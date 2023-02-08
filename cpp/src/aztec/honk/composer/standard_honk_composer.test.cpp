#include "standard_honk_composer.hpp"
#include "common/assert.hpp"
#include "honk/sumcheck/relations/relation.hpp"
#include "numeric/uint256/uint256.hpp"
#include "proof_system/flavor/flavor.hpp"
#include <cstdint>
#include <honk/proof_system/prover.hpp>
#include <honk/sumcheck/polynomials/multivariates.hpp>
#include <honk/sumcheck/sumcheck_round.hpp>
#include <honk/sumcheck/relations/grand_product_computation_relation.hpp>
#include <honk/sumcheck/relations/grand_product_initialization_relation.hpp>
#include <honk/utils/public_inputs.hpp>

#include <cstdint>
#include <gtest/gtest.h>

#pragma GCC diagnostic ignored "-Wunused-variable"

using namespace honk;

namespace test_standard_honk_composer {
/**
 * @brief The goal of this test is to check that the sigma permutation vectors for honk are generated correctly.
 *
 * @details Specifically:
 * 1) That they are indeed a permutation of all initial indices
 * 2) That if the permutation argument is computed with witness values, the values from the identity permutation and
 * sigma permutation are equal
 */
TEST(StandardHonkComposer, SigmaIDCorrectness)
{
    auto test_permutation = [](StandardHonkComposer& composer) {
        auto proving_key = composer.compute_proving_key();
        const auto n = proving_key->circuit_size;

        auto public_inputs = composer.circuit_constructor.get_public_inputs();
        auto num_public_inputs = public_inputs.size();
        auto num_gates = composer.circuit_constructor.get_num_gates();

        // Using the same random beta and gamma as in the permutation argument
        barretenberg::fr beta = barretenberg::fr::random_element();
        barretenberg::fr gamma = barretenberg::fr::random_element();

        barretenberg::fr left = barretenberg::fr::one();
        barretenberg::fr right = barretenberg::fr::one();

        // Let's check that indices are the same and nothing is lost, first
        for (size_t j = 0; j < composer.program_width; ++j) {
            std::string index = std::to_string(j + 1);
            const auto& sigma_j = proving_key->polynomial_cache.get("sigma_" + index + "_lagrange");
            for (size_t i = 0; i < n; ++i) {
                left *= (gamma + j * n + i);
                right *= (gamma + sigma_j[i]);
            }
            // Ensure that the public inputs cycles are correctly broken
            // and fix the cycle by adding the extra terms
            if (j == 0) {
                for (size_t i = 0; i < num_public_inputs; ++i) {
                    EXPECT_EQ(sigma_j[i], -fr(i + 1));
                    left *= (gamma - (i + 1));
                    right *= (gamma + (n + i));
                }
            }
        }

        EXPECT_EQ(left, right);

        left = barretenberg::fr::one();
        right = barretenberg::fr::one();

        // Now let's check that witness values correspond to the permutation
        composer.compute_witness();

        for (size_t j = 0; j < composer.program_width; ++j) {
            std::string index = std::to_string(j + 1);
            const auto& permutation_polynomial = proving_key->polynomial_cache.get("sigma_" + index + "_lagrange");
            const auto& witness_polynomial = proving_key->polynomial_cache.get("w_" + index + "_lagrange");
            const auto& id_polynomial = proving_key->polynomial_cache.get("id_" + index + "_lagrange");
            // left = ∏ᵢ,ⱼ(ωᵢ,ⱼ + β⋅ind(i,j) + γ)
            // right = ∏ᵢ,ⱼ(ωᵢ,ⱼ + β⋅σ(i,j) + γ)
            for (size_t i = 0; i < proving_key->circuit_size; ++i) {
                const auto current_witness = witness_polynomial[i];
                left *= current_witness + beta * id_polynomial[i] + gamma;
                right *= current_witness + beta * permutation_polynomial[i] + gamma;
            }
            // check that the first rows are correctly set to handle public inputs.
            for (size_t i = 0; i < num_public_inputs; ++i) {
                if ((j == 0) || (j == 1)) {
                    EXPECT_EQ(witness_polynomial[i], public_inputs[i]);
                } else {
                    EXPECT_EQ(witness_polynomial[i], 0);
                }
            }
            // Check that the last rows are all 0
            for (size_t i = num_public_inputs + num_gates; i < n; ++i) {
                EXPECT_EQ(witness_polynomial[i], 0);
            }
        }

        // test correctness of the public input delta
        auto delta = compute_public_input_delta<fr>(public_inputs, beta, gamma, n);
        EXPECT_EQ(left / right, delta);

        for (size_t i = 0; i < num_public_inputs; ++i) {
            left *= public_inputs[i] - beta * (i + 1) + gamma;
            right *= public_inputs[i] + beta * (n + i) + gamma;
        }
        EXPECT_EQ(left, right);
    };

    StandardHonkComposer composer = StandardHonkComposer();
    fr a = fr::one();
    uint32_t a_idx = composer.add_variable(a);
    fr b = fr::one();
    fr c = a + b;
    uint32_t b_idx = composer.add_variable(b);
    uint32_t c_idx = composer.add_variable(c);
    fr d = a + c;
    uint32_t d_idx = composer.add_public_variable(d);

    uint32_t e_idx = composer.put_constant_variable(d);
    composer.assert_equal(e_idx, d_idx, "");

    composer.create_add_gate({ a_idx, b_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
    composer.create_add_gate({ d_idx, c_idx, a_idx, fr::one(), fr::neg_one(), fr::neg_one(), fr::zero() });
    composer.create_add_gate({ a_idx, b_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
    composer.create_add_gate({ a_idx, b_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
    composer.create_add_gate({ b_idx, a_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
    for (size_t i = 0; i < 30; ++i) {
        composer.create_add_gate({ a_idx, b_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
    }

    test_permutation(composer);
}

/**
 * @brief Check the correctness of lagrange polynomials generated during proving key computation
 *
 */
TEST(StandardHonkComposer, LagrangeCorrectness)
{
    // Create a composer and a dummy circuit with a few gates
    StandardHonkComposer composer = StandardHonkComposer();
    fr a = fr::one();
    uint32_t a_idx = composer.add_variable(a);
    fr b = fr::one();
    fr c = a + b;
    fr d = a + c;
    uint32_t b_idx = composer.add_variable(b);
    uint32_t c_idx = composer.add_variable(c);
    uint32_t d_idx = composer.add_variable(d);
    for (size_t i = 0; i < 16; i++) {
        composer.create_add_gate({ a_idx, b_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
        composer.create_add_gate({ d_idx, c_idx, a_idx, fr::one(), fr::neg_one(), fr::neg_one(), fr::zero() });
    }
    // Generate proving key
    auto proving_key = composer.compute_proving_key();
    // Generate a random polynomial
    barretenberg::polynomial random_polynomial = barretenberg::polynomial(proving_key->circuit_size);
    for (size_t i = 0; i < proving_key->circuit_size; i++) {
        random_polynomial[i] = barretenberg::fr::random_element();
    }
    // Compute inner product of random polynomial and the first lagrange polynomial
    barretenberg::polynomial first_lagrange_polynomial = proving_key->polynomial_cache.get("L_first_lagrange");
    barretenberg::fr first_product(0);
    for (size_t i = 0; i < proving_key->circuit_size; i++) {
        first_product += random_polynomial[i] * first_lagrange_polynomial[i];
    }
    EXPECT_EQ(first_product, random_polynomial[0]);

    // Compute inner product of random polynomial and the last lagrange polynomial
    barretenberg::polynomial last_lagrange_polynomial = proving_key->polynomial_cache.get("L_last_lagrange");
    barretenberg::fr last_product(0);
    for (size_t i = 0; i < proving_key->circuit_size; i++) {
        last_product += random_polynomial[i] * last_lagrange_polynomial[i];
    }
    EXPECT_EQ(last_product, random_polynomial[proving_key->circuit_size - 1]);
}

/**
 * @brief Test that the assert_equal method in composer is working as intended
 *
 * @details We show equality of witness values through permutation arguments, so the assert_equal method changes the
 * underlying variable structure. If we bind two real variables through it, we expect their wire copy cycles to be
 * merged.
 * In this test we create two almost identical circuits. They differ because one
 */
TEST(StandardHonkComposer, AssertEquals)
{
    /**
     * @brief A function that creates a simple circuit with repeated gates, leading to large permutation cycles
     *
     */
    auto create_simple_circuit = [](auto& composer) {
        fr a = fr::one();
        uint32_t a_idx = composer.add_variable(a);
        fr b = fr::one();
        fr c = a + b;
        uint32_t b_idx = composer.add_variable(b);
        uint32_t c_idx = composer.add_variable(c);

        for (size_t i = 0; i < 10; i++) {
            composer.create_add_gate({ a_idx, b_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
            composer.create_add_gate({ b_idx, a_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
        }
        return std::make_tuple(a_idx, b_idx);
    };
    /**
     * @brief A function that computes the largest cycle from the sigma permutation generated by the composer
     *
     */
    auto get_maximum_cycle = [](auto& composer) {
        // Compute the proving key for sigma polynomials
        auto proving_key = composer.compute_proving_key();
        auto permutation_length = composer.program_width * proving_key->circuit_size;
        std::vector<polynomial> sigma_polynomials;

        // Put the sigma polynomials into a vector for easy access
        for (size_t i = 0; i < composer.program_width; i++) {
            std::string index = std::to_string(i + 1);
            sigma_polynomials.push_back(proving_key->polynomial_cache.get("sigma_" + index + "_lagrange"));
        }

        // Let's compute the maximum cycle
        size_t maximum_cycle = 0;

        std::vector<bool> visited_indices;

        visited_indices.resize(permutation_length, false);

        for (size_t i = 0; i < permutation_length;) {
            // Jump to first unvisited member in the cycle
            // We check that i is limited by permutation_length
            while (visited_indices[i] && (i < permutation_length)) {
                i++;
            }
            if (i >= permutation_length) {
                break;
            }
            auto starting_element = i;
            auto next_element_big =
                static_cast<uint256_t>(sigma_polynomials[i / proving_key->circuit_size][i % proving_key->circuit_size]);
            EXPECT_LE(next_element_big, uint256_t(UINT32_MAX));
            auto next_element = static_cast<size_t>(next_element_big.data[0]);
            size_t cycle_length = 1;
            visited_indices[i] = true;

            // Jump through the cycle untill we reach the start or the permutation length exceeds the possible maximum
            while ((next_element != starting_element) && cycle_length < (permutation_length + 1)) {
                // Update cycle length and visited index infromation
                cycle_length++;
                visited_indices[next_element] = true;
                // Get next index
                next_element_big = static_cast<uint256_t>(sigma_polynomials[next_element / proving_key->circuit_size]
                                                                           [next_element % proving_key->circuit_size]);
                EXPECT_LE(next_element_big, uint256_t(UINT32_MAX));
                next_element = static_cast<size_t>(next_element_big.data[0]);
            }
            // If cycle_length is larger than permutation length, then instead of just a cycle we have a runway,too,
            // which is incorrect
            EXPECT_LE(cycle_length, permutation_length);

            // Update the maximum cycle
            if (cycle_length > maximum_cycle) {
                maximum_cycle = cycle_length;
            }
        }
        return maximum_cycle;
    };

    // Get 2 circuits
    StandardHonkComposer composer_no_assert_equal = StandardHonkComposer();
    StandardHonkComposer composer_with_assert_equal = StandardHonkComposer();

    // Construct circuits
    create_simple_circuit(composer_no_assert_equal);
    auto assert_eq_params = create_simple_circuit(composer_with_assert_equal);

    // Use assert_equal on one of them
    composer_with_assert_equal.assert_equal(std::get<0>(assert_eq_params),
                                            std::get<1>(assert_eq_params),
                                            "Equality asssertion in standard honk composer test");

    // Check that the maximum cycle in the one, where we used assert_equal, is twice as long
    EXPECT_EQ(get_maximum_cycle(composer_with_assert_equal), get_maximum_cycle(composer_no_assert_equal) * 2);
}

TEST(StandardHonkComposer, VerificationKeyCreation)
{
    // Create a composer and a dummy circuit with a few gates
    StandardHonkComposer composer = StandardHonkComposer();
    fr a = fr::one();
    uint32_t a_idx = composer.add_variable(a);
    fr b = fr::one();
    fr c = a + b;
    fr d = a + c;
    uint32_t b_idx = composer.add_variable(b);
    uint32_t c_idx = composer.add_variable(c);
    uint32_t d_idx = composer.add_variable(d);
    for (size_t i = 0; i < 16; i++) {
        composer.create_add_gate({ a_idx, b_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
        composer.create_add_gate({ d_idx, c_idx, a_idx, fr::one(), fr::neg_one(), fr::neg_one(), fr::zero() });
    }
    auto verification_key = composer.compute_verification_key();
    // There is nothing we can really check apart from the fact that constraint selectors and permutation selectors were
    // committed to, we simply check that the verification key now contains the appropriate number of constraint and
    // permutation selector commitments. This method should work with any future arithemtization.
    EXPECT_EQ(verification_key->commitments.size(),
              composer.circuit_constructor.selectors.size() + composer.program_width * 2 + 2);
}

/**
 * @brief A test taking sumcheck relations and applying them to the witness and selector polynomials to ensure that the
 * realtions are correct.
 *
 * TODO(kesha): We'll have to update this function once we add zk, since the relation will be incorrect for he first few
 * indices
 *
 */
TEST(StandardHonkComposer, SumcheckRelationCorrectness)
{
    // Create a composer and a dummy circuit with a few gates
    StandardHonkComposer composer = StandardHonkComposer();
    fr a = fr::one();
    // Using the public variable to check that public_input_delta is computed and added to the relation correctly
    uint32_t a_idx = composer.add_public_variable(a);
    fr b = fr::one();
    fr c = a + b;
    fr d = a + c;
    uint32_t b_idx = composer.add_variable(b);
    uint32_t c_idx = composer.add_variable(c);
    uint32_t d_idx = composer.add_variable(d);
    for (size_t i = 0; i < 16; i++) {
        composer.create_add_gate({ a_idx, b_idx, c_idx, fr::one(), fr::one(), fr::neg_one(), fr::zero() });
        composer.create_add_gate({ d_idx, c_idx, a_idx, fr::one(), fr::neg_one(), fr::neg_one(), fr::zero() });
    }
    // Create a prover (it will compute proving key and witness)
    auto prover = composer.create_prover();

    // Generate beta and gamma
    fr beta = fr::random_element();
    fr gamma = fr::random_element();
    fr zeta = fr::random_element();

    // Compute grand product polynomial (now all the necessary polynomials are inside the proving key)
    prover.compute_grand_product_polynomial(beta, gamma);

    // Compute public input delta
    const auto public_inputs = composer.circuit_constructor.get_public_inputs();
    auto public_input_delta = compute_public_input_delta<fr>(public_inputs, beta, gamma, prover.key->circuit_size);

    sumcheck::RelationParameters<fr> params{
        .zeta = zeta,
        .alpha = fr::one(),
        .beta = beta,
        .gamma = gamma,
        .public_input_delta = public_input_delta,
    };

    constexpr size_t num_polynomials = bonk::StandardArithmetization::NUM_POLYNOMIALS;
    // Retrieve polynomials from proving key
    polynomial w_1 = prover.key->polynomial_cache.get("w_1_lagrange");
    polynomial w_2 = prover.key->polynomial_cache.get("w_2_lagrange");
    polynomial w_3 = prover.key->polynomial_cache.get("w_3_lagrange");
    polynomial z_perm = prover.key->polynomial_cache.get("z_perm_lagrange");
    polynomial q_m = prover.key->polynomial_cache.get("q_m_lagrange");
    polynomial q_1 = prover.key->polynomial_cache.get("q_1_lagrange");
    polynomial q_2 = prover.key->polynomial_cache.get("q_2_lagrange");
    polynomial q_3 = prover.key->polynomial_cache.get("q_3_lagrange");
    polynomial q_c = prover.key->polynomial_cache.get("q_c_lagrange");
    polynomial sigma_1 = prover.key->polynomial_cache.get("sigma_1_lagrange");
    polynomial sigma_2 = prover.key->polynomial_cache.get("sigma_2_lagrange");
    polynomial sigma_3 = prover.key->polynomial_cache.get("sigma_3_lagrange");
    polynomial id_1 = prover.key->polynomial_cache.get("id_1_lagrange");
    polynomial id_2 = prover.key->polynomial_cache.get("id_2_lagrange");
    polynomial id_3 = prover.key->polynomial_cache.get("id_3_lagrange");
    polynomial L_first = prover.key->polynomial_cache.get("L_first_lagrange");
    polynomial L_last = prover.key->polynomial_cache.get("L_last_lagrange");

    // Create an array of spans to the underlying polynomials to more easily
    // get the transposition.
    // Ex: polynomial_spans[3][i] returns the i-th coefficient of the third polynomial
    // in the list below
    std::array<std::span<fr>, num_polynomials> polynomial_spans{ { w_1,
                                                                   w_2,
                                                                   w_3,
                                                                   z_perm,
                                                                   z_perm.shifted(),
                                                                   q_m,
                                                                   q_1,
                                                                   q_2,
                                                                   q_3,
                                                                   q_c,
                                                                   sigma_1,
                                                                   sigma_2,
                                                                   sigma_3,
                                                                   id_1,
                                                                   id_2,
                                                                   id_3,
                                                                   L_first,
                                                                   L_last } };

    // Construct the round for applying sumcheck relations and results for storing computed results
    auto relations = std::tuple(honk::sumcheck::ArithmeticRelation<fr>(),
                                honk::sumcheck::GrandProductComputationRelation<fr>(),
                                honk::sumcheck::GrandProductInitializationRelation<fr>());

    // Check all relations at all indices.
    for (size_t i = 0; i < prover.key->circuit_size; i++) {

        // Compute an array containing all the evaluations at a given row i
        // Ex: transposed[3] returns the i-th coefficient of the third polynomial
        std::array<fr, num_polynomials> transposed;
        for (size_t j = 0; j < num_polynomials; ++j) {
            transposed[j] = polynomial_spans[j][i];
        }

        // For each relation, call the `accumulate_relation_evaluation` over all witness/selector values at the
        // i-th row/vertex of the hypercube.
        // We always set the accumulator to 0, so that
        // result = 0 + C(transposed)*1, which we expect will equal 0.
        fr result = 0;
        std::get<0>(relations).accumulate_relation_evaluation(result, transposed, params, 1);
        EXPECT_EQ(result, 0);

        result = 0;
        std::get<1>(relations).accumulate_relation_evaluation(result, transposed, params, 1);
        EXPECT_EQ(result, 0);

        result = 0;
        std::get<2>(relations).accumulate_relation_evaluation(result, transposed, params, 1);
        EXPECT_EQ(result, 0);
    }
}

TEST(StandardHonkComposer, BaseCase)
{
    auto composer = StandardHonkComposer();
    fr a = 1;
    composer.circuit_constructor.add_variable(a);

    auto prover = composer.create_unrolled_prover();
    waffle::plonk_proof proof = prover.construct_proof();
    auto verifier = composer.create_unrolled_verifier();
    bool verified = verifier.verify_proof(proof);
    ASSERT_TRUE(verified);
}

TEST(StandardHonkComposer, TwoGates)
{
    auto run_test = [](bool expect_verified) {
        auto composer = StandardHonkComposer();

        // 1 + 1 - 2 = 0
        uint32_t w_l_1_idx;
        if (expect_verified) {
            w_l_1_idx = composer.circuit_constructor.add_variable(1);
        } else {
            w_l_1_idx = composer.circuit_constructor.add_variable(0);
        }
        uint32_t w_r_1_idx = composer.circuit_constructor.add_variable(1);
        uint32_t w_o_1_idx = composer.circuit_constructor.add_variable(2);
        composer.create_add_gate({ w_l_1_idx, w_r_1_idx, w_o_1_idx, 1, 1, -1, 0 });

        // 2 * 2 - 4 = 0
        uint32_t w_l_2_idx = composer.circuit_constructor.add_variable(2);
        uint32_t w_r_2_idx = composer.circuit_constructor.add_variable(2);
        uint32_t w_o_2_idx = composer.circuit_constructor.add_variable(4);
        composer.create_mul_gate({ w_l_2_idx, w_r_2_idx, w_o_2_idx, 1, -1, 0 });

        auto prover = composer.create_unrolled_prover();

        waffle::plonk_proof proof = prover.construct_proof();
        auto verifier = composer.create_unrolled_verifier();
        bool verified = verifier.verify_proof(proof);
        EXPECT_EQ(verified, expect_verified);
    };

    run_test(/* expect_verified=*/true);
    run_test(/* expect_verified=*/false);
}
} // namespace test_standard_honk_composer
