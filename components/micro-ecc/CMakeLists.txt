# kmackay/micro-ecc (BSD-2-Clause) as a git submodule, built with only the
# curve the EcoFlow key exchange uses: secp160r1, which mbedTLS does not offer.
idf_component_register(SRCS "micro-ecc/uECC.c" INCLUDE_DIRS "micro-ecc")
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    uECC_SUPPORTS_secp160r1=1
    uECC_SUPPORTS_secp192r1=0
    uECC_SUPPORTS_secp224r1=0
    uECC_SUPPORTS_secp256r1=0
    uECC_SUPPORTS_secp256k1=0
    uECC_SUPPORT_COMPRESSED_POINT=0)

# ESP-IDF's Bluetooth component bundles tinycrypt, a micro-ecc derivative that
# exports the same uECC_* names with a different API and only secp256r1. Give
# ours distinct link names; callers still write the usual uECC_* names.
set(uecc_public
    uECC_compress uECC_compute_public_key uECC_curve_private_key_size
    uECC_curve_public_key_size uECC_decompress uECC_get_rng uECC_make_key
    uECC_secp160r1 uECC_set_rng uECC_shared_secret uECC_sign
    uECC_sign_deterministic uECC_sign_with_k uECC_valid_public_key uECC_verify)
foreach(name IN LISTS uecc_public)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ${name}=canary_${name})
endforeach()
