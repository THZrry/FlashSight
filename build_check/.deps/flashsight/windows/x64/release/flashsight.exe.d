{
    files = {
        [[build_check\.objs\flashsight\windows\x64\release\src\main.cpp.obj]],
        [[build_check\windows\x64\release\fs_core.lib]],
        [[build_check\windows\x64\release\fs_ui.lib]]
    },
    values = {
        [[C:\Program Files\mingw64\bin\x86_64-w64-mingw32-g++]],
        {
            "-m64",
            [[-Lbuild_check\windows\x64\release]],
            "-lfs_core",
            "-lfs_ui",
            "-lws2_32",
            "-luser32",
            "-lshell32",
            "-lole32",
            "-luuid",
            [[C:\aaa\FlashSight/3rdparty/mbedtls-3.6.4/build/library/libmbedtls.a]],
            [[C:\aaa\FlashSight/3rdparty/mbedtls-3.6.4/build/library/libmbedx509.a]],
            [[C:\aaa\FlashSight/3rdparty/mbedtls-3.6.4/build/library/libmbedcrypto.a]],
            [[C:\aaa\FlashSight/3rdparty/mbedtls-3.6.4/build/3rdparty/everest/libeverest.a]],
            [[C:\aaa\FlashSight/3rdparty/mbedtls-3.6.4/build/3rdparty/p256-m/libp256m.a]],
            "-lbcrypt",
            "-lcrypt32"
        }
    }
}