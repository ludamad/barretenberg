
#include "./verifier.hpp"
#include <common/throw_or_abort.hpp>
#include <ecc/curves/bn254/fr.hpp>
#include <polynomials/polynomial_arithmetic.hpp>
#include "honk/pcs/commitment_key.hpp"
#include "honk/pcs/gemini/gemini.hpp"
#include "honk/pcs/kzg/kzg.hpp"
#include "honk/utils/public_inputs.hpp"
#include "honk/sumcheck/sumcheck.hpp"
#include "honk/sumcheck/relations/relation.hpp"
#include "honk/sumcheck/relations/arithmetic_relation.hpp"
#include "honk/sumcheck/relations/grand_product_computation_relation.hpp"
#include "honk/sumcheck/relations/grand_product_initialization_relation.hpp"
#include "numeric/bitop/get_msb.hpp"
#include "proof_system/flavor/flavor.hpp"
#include <plonk/proof_system/constants.hpp>

#include <string>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace honk {
template <typename program_settings>
Verifier<program_settings>::Verifier(std::shared_ptr<bonk::verification_key> verifier_key)
    : key(verifier_key)
{}

template <typename program_settings>
Verifier<program_settings>::Verifier(Verifier&& other)
    : key(other.key)
    , kate_verification_key(std::move(other.kate_verification_key))
{}

template <typename program_settings> Verifier<program_settings>& Verifier<program_settings>::operator=(Verifier&& other)
{
    key = other.key;
    kate_verification_key = (std::move(other.kate_verification_key));
    kate_g1_elements.clear();
    kate_fr_elements.clear();
    return *this;
}

/**
* @brief This function verifies a Honk proof for given program settings.
*
* @details A Standard Honk proof contains the following:
    Multilinear evaluations:
        w_i(X),        i = 1,2,3
        sigma_i(X),    i = 1,2,3
        q_i(X),        i = 1,2,3,4,5
        z_perm(X),
        L_0(X),
        id(X)

    Univariate evaluations:
        a_0 = Fold_{-r}^(0)(-r),
        a_l = Fold^(l)(-r^{2^l}), i = 1,...,d-1

    Univariate polynomials (evaluations over MAX_RELATION_LENGTH-many points):
        S_l, l = 0,...,d-1

    Commitments:
        [w_i]_1,        i = 1,2,3
        [z_perm]_1,
        [Fold^(l)]_1,   l = 1,...,d-1
        [Q]_1,
        [W]_1
*/
template <typename program_settings> bool Verifier<program_settings>::verify_proof(const plonk::proof& proof)
{
    using FF = typename program_settings::fr;
    using Commitment = barretenberg::g1::element;
    using CommitmentAffine = barretenberg::g1::affine_element;
    using Gemini = pcs::gemini::MultilinearReductionScheme<pcs::kzg::Params>;
    using Shplonk = pcs::shplonk::SingleBatchOpeningScheme<pcs::kzg::Params>;
    using KZG = pcs::kzg::UnivariateOpeningScheme<pcs::kzg::Params>;
    const size_t NUM_POLYNOMIALS = bonk::StandardArithmetization::NUM_POLYNOMIALS;
    const size_t NUM_UNSHIFTED = bonk::StandardArithmetization::NUM_UNSHIFTED_POLYNOMIALS;
    const size_t NUM_PRECOMPUTED = bonk::StandardArithmetization::NUM_PRECOMPUTED_POLYNOMIALS;

    constexpr auto width = program_settings::program_width;

    VerifierTranscript<typename program_settings::fr> transcript{ proof.proof_data };

    // TODO(Adrian): Change the initialization of the transcript to take the VK hash? Also need to add the
    // commitments...
    auto circuit_size = transcript.template receive_from_prover<uint32_t>("circuit_size");
    [[maybe_unused]] auto public_input_size = transcript.template receive_from_prover<uint32_t>("public_input_size");

    if (circuit_size != key->circuit_size) {
        return false;
    }
    if (public_input_size != key->num_public_inputs) {
        return false;
    }

    std::vector<FF> public_inputs;
    for (size_t i = 0; i < public_input_size; ++i) {
        auto public_input_i = transcript.template receive_from_prover<FF>("public_inputs_" + std::to_string(i));
        public_inputs.emplace_back(public_input_i);
    }

    std::array<CommitmentAffine, width> wire_commitments;
    for (size_t i = 0; i < width; ++i) {
        wire_commitments[i] = transcript.template receive_from_prover<CommitmentAffine>("W_" + std::to_string(i + 1));
    }

    auto [beta, gamma] = transcript.get_challenges("beta", "gamma");

    auto public_input_delta = compute_public_input_delta<FF>(public_inputs, beta, gamma, circuit_size);

    auto z_permutation_commitment = transcript.template receive_from_prover<CommitmentAffine>("Z_PERM");

    // // TODO(Cody): Compute some basic public polys like id(X), pow(X), and any required Lagrange polys

    sumcheck::RelationParameters<FF> relation_parameters{
        .beta = beta,
        .gamma = gamma,
        .public_input_delta = public_input_delta,
    };

    // Execute Sumcheck Verifier
    auto sumcheck = sumcheck::Sumcheck<FF,
                                       sumcheck::ArithmeticRelation,
                                       sumcheck::GrandProductComputationRelation,
                                       sumcheck::GrandProductInitializationRelation>(circuit_size, relation_parameters);
    auto sumcheck_result = sumcheck.execute_verifier(transcript);

    if (!sumcheck_result.has_value()) {
        return false;
    }

    // Get opening point and vector of multivariate evaluations produced by Sumcheck
    // - Multivariate opening point u = (u_0, ..., u_{d-1})
    auto [opening_point, multivariate_evaluations] = *sumcheck_result;
    // Execute Gemini/Shplonk verification:

    // Construct inputs for Gemini verifier:
    // - Multivariate opening point u = (u_0, ..., u_{d-1})
    // - batched unshifted and to-be-shifted polynomial commitments
    auto batched_commitment_unshifted = Commitment::zero();
    auto batched_commitment_to_be_shifted = Commitment::zero();

    // Compute powers of batching challenge rho
    FF rho = transcript.get_challenge("rho");
    std::vector<FF> rhos = Gemini::powers_of_rho(rho, NUM_POLYNOMIALS);

    // Compute batched multivariate evaluation
    FF batched_evaluation = FF::zero();
    for (size_t i = 0; i < NUM_POLYNOMIALS; ++i) {
        batched_evaluation += multivariate_evaluations[i] * rhos[i];
    }

    // Construct batched commitment for NON-shifted polynomials
    for (size_t i = 0; i < NUM_PRECOMPUTED; ++i) {
        // if precomputed, commitment comes from verification key
        Commitment commitment = key->commitments[bonk::StandardArithmetization::ENUM_TO_COMM[i]];
        batched_commitment_unshifted += commitment * rhos[i];
    }
    // add wire commitments and evals
    for (size_t i = 0; i < width; ++i) {
        // if witness, commitment comes from prover
        Commitment commitment = wire_commitments[i];
        batched_commitment_unshifted += commitment * rhos[NUM_PRECOMPUTED + i];
    }

    // Construct batched commitment for to-be-shifted polynomials
    batched_commitment_unshifted += z_permutation_commitment * rhos[NUM_PRECOMPUTED + width];
    batched_commitment_to_be_shifted = z_permutation_commitment * rhos[NUM_UNSHIFTED];

    // Produce a Gemini claim consisting of:
    // - d+1 commitments [Fold_{r}^(0)], [Fold_{-r}^(0)], and [Fold^(l)], l = 1:d-1
    // - d+1 evaluations a_0_pos, and a_l, l = 0:d-1
    auto gemini_claim = Gemini::reduce_verify(
        opening_point, batched_evaluation, batched_commitment_unshifted, batched_commitment_to_be_shifted, transcript);

    // Produce a Shplonk claim: commitment [Q] - [Q_z], evaluation zero (at random challenge z)
    auto shplonk_claim = Shplonk::reduce_verify(gemini_claim, transcript);

    // Aggregate inputs [Q] - [Q_z] and [W] into an 'accumulator' (can perform pairing check on result)
    auto kzg_claim = KZG::reduce_verify(shplonk_claim, transcript);

    // Do final pairing check
    bool pairing_result = kzg_claim.verify(kate_verification_key);

    bool result = sumcheck_result && pairing_result;

    return result;
}

template class Verifier<honk::standard_verifier_settings>;

} // namespace honk