{
    files = {
        [[src\common\fs_net_http.cpp]]
    },
    depfiles_format = "gcc",
    values = {
        [[C:\Program Files\mingw64\bin\x86_64-w64-mingw32-g++]],
        {
            "-m64",
            "-fvisibility=hidden",
            "-fvisibility-inlines-hidden",
            "-O3",
            "-std=c++14",
            "-Isrc",
            [[-Isrc\common]],
            [[-Isrc\core]],
            [[-Isrc\storage]],
            [[-Isrc\provider]],
            [[-Isrc\agent]],
            [[-Isrc\tools]],
            [[-I3rdparty\filesystem-1.5.14\include]],
            [[-I3rdparty\json]],
            [[-I3rdparty\dtl-1.20]],
            [[-I3rdparty\md4c-master\src]],
            [[-I3rdparty\httplib]],
            [[-I3rdparty\mbedtls-3.6.4\include]],
            [[-I3rdparty\qjs-regex]],
            [[-I3rdparty\reproc\include]],
            [[-I3rdparty\whaleui\include]],
            "-D_WIN32_WINNT=0x0A00",
            "-DWIN32_LEAN_AND_MEAN",
            "-DFS_UI_WHALEUI",
            "-DCPPHTTPLIB_MBEDTLS_SUPPORT",
            "-fexceptions",
            "-Wall",
            "-DNDEBUG"
        }
    },
    depfiles = "fs_net_http.o: src\\common\\fs_net_http.cpp src\\common\\fs_net_http.hpp  src\\common\\fs_net.hpp src\\common\\fs_error.hpp  3rdparty\\json/nlohmann/json.hpp 3rdparty\\json/nlohmann/../json.hpp  3rdparty\\httplib/httplib.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/error.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/build_info.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/mbedtls_config.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/config_psa.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_legacy.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_adjust_config_synonyms.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_adjust_config_dependencies.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/config_adjust_psa_superset_legacy.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/config_adjust_psa_from_legacy.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_adjust_config_key_pair_types.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_adjust_auto_enabled.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/config_adjust_legacy_crypto.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/config_adjust_x509.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/config_adjust_ssl.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/check_config.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/net_sockets.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/private_access.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ssl.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/platform_util.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/platform_time.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/bignum.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ecp.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ssl_ciphersuites.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/pk.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/md.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/rsa.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ecdsa.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_platform.h  3rdparty\\mbedtls-3.6.4\\include/psa/build_info.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_types.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_values.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_sizes.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_struct.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_driver_contexts_primitives.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_driver_common.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_sizes.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_builtin_primitives.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/md5.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ripemd160.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/sha1.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/sha256.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/sha512.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/sha3.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/cipher.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_driver_contexts_composites.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_builtin_composites.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/cmac.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/gcm.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ccm.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/chachapoly.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/poly1305.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/chacha20.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ecjpake.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_driver_contexts_key_derivation.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_builtin_key_derivation.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_extra.h  3rdparty\\mbedtls-3.6.4\\include/psa/crypto_compat.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/x509_crt.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/x509.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/asn1.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/x509_crl.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/dhm.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ecdh.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/oid.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/version.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/ctr_drbg.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/aes.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/entropy.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/md.h  3rdparty\\mbedtls-3.6.4\\include/mbedtls/entropy.h\
"
}