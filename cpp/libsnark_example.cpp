#include "libsnark/gadgetlib1/gadget.hpp"
#include "libsnark/gadgetlib1/protoboard.hpp"
#include "libff/common/default_types/ec_pp.hpp"
#include "libsnark/gadgetlib1/gadgets/hashes/sha256/sha256_components.hpp"
#include "libsnark/gadgetlib1/gadgets/hashes/sha256/sha256_gadget.hpp"

#include "gadget.h"
#include "gadget_generated.h"
#include "libsnark_integration.hpp"

using std::vector;
using namespace libsnark;
using namespace libff;
using namespace standard_gadget;


class sha256_gadget : standard_libsnark_gadget {
private:
    digest_variable<FieldT> left, right, output;
    sha256_two_to_one_hash_gadget<FieldT> hasher;

public:
    protoboard<FieldT> pb;

    protoboard<FieldT> &borrow_protoboard() { return pb; }

    sha256_gadget(const GadgetInstance *instance) :
            left(pb, 256, "left"),
            right(pb, 256, "right"),
            output(pb, 256, "output"),
            hasher(pb, left, right, output, "sha256") {

        // Sanity check the function signature.
        assert(instance->incoming_variable_ids()->size() == num_inputs());
        assert(instance->outgoing_variable_ids()->size() == num_outputs());
    }

    size_t num_inputs() { return left.bits.size() + left.bits.size(); }

    size_t num_outputs() { return output.bits.size(); }

    void generate_r1cs_constraints() {
        left.generate_r1cs_constraints();
        right.generate_r1cs_constraints();
        output.generate_r1cs_constraints();
        hasher.generate_r1cs_constraints();
    }

    vector<FieldT> generate_r1cs_witness(const vector<FieldT> &in_elements) {
        assert(in_elements.size() == num_inputs());
        size_t half_inputs = in_elements.size() / 2;

        // Interpret inputs as bits.
        bit_vector left_bits(half_inputs);
        bit_vector right_bits(half_inputs);

        for (size_t i = 0; i < half_inputs; i++) {
            left_bits[i] = (in_elements[i] == 1);
            right_bits[i] = (in_elements[half_inputs + i] == 1);
        }

        left.generate_r1cs_witness(left_bits);
        right.generate_r1cs_witness(right_bits);
        hasher.generate_r1cs_witness();

        return output.bits.get_vals(pb);
    }
};


extern "C"
bool sha256_gadget_call(
        unsigned char *request_buf,
        gadget_callback_t result_stream_callback,
        void *result_stream_context,
        gadget_callback_t response_callback,
        void *response_context
) {
    auto root = GetSizePrefixedRoot(request_buf);

    if (root->message_type() != Message_ComponentCall) {
        return return_error(response_callback, response_context, "Unexpected message");
    }

    const ComponentCall *call = root->message_as_ComponentCall();
    const GadgetInstance *instance = call->instance();

    libff::alt_bn128_pp::init_public_params();

    sha256_gadget gadget(instance);

    // Instance reduction.
    if (call->generate_r1cs()) {
        gadget.generate_r1cs_constraints();

        // Report constraints.
        if (result_stream_callback != nullptr) {
            auto constraints_msg = serialize_protoboard_constraints(instance, gadget.borrow_protoboard());
            result_stream_callback(result_stream_context, constraints_msg.GetBufferPointer());
            // Releasing constraints_msg...
        }
    }

    // Witness reduction.
    vector<FieldT> out_elements;

    if (call->generate_assignment()) {
        vector<FieldT> in_elements = deserialize_incoming_elements(call);

        out_elements = gadget.generate_r1cs_witness(in_elements);

        // Report assignment to generated local variables.
        if (result_stream_callback != nullptr) {
            auto assignment_msg = serialize_protoboard_local_assignment(instance, gadget.borrow_protoboard());
            result_stream_callback(result_stream_context, assignment_msg.GetBufferPointer());
            // Releasing assignment_msg...
        }
    }

    // Response.
    FlatBufferBuilder builder;

    uint64_t num_local_vars = gadget.borrow_protoboard().num_variables() - gadget.num_inputs() - gadget.num_outputs();
    uint64_t free_variable_id_after = instance->free_variable_id_before() + num_local_vars;
    auto maybe_out_elements = call->generate_assignment() ? serialize_elements(builder, out_elements) : 0;

    auto response = CreateComponentReturn(
            builder,
            free_variable_id_after,
            0, // No custom info.
            0, // No error.
            maybe_out_elements);

    builder.FinishSizePrefixed(CreateRoot(builder, Message_ComponentReturn, response.Union()));

    if (response_callback != nullptr) {
        return response_callback(response_context, builder.GetBufferPointer());
    }

    return true;
}