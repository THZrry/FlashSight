{
    files = {
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\common\fs_i18n.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\common\fs_json.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\common\fs_log.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\term\fs_vt.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\ui\ui_app.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\ui\ui_backend.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\ui\ui_components.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\ui\ui_dom_null.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\ui\ui_dom_wasm.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\ui\ui_http_backend.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\ui\ui_main_wasm.cpp.o]],
        [[build_whale\.objs\fs_ui_wasm\wasm\x64\release\src\ui\ui_vdom.cpp.o]]
    },
    values = {
        [[C:\Program Files\emscripten\emscripten\em++.exe]],
        {
            [[-LC:\aaa\WhaleUI\build\mingw\x86_64\release]],
            "-O3",
            "-lembind",
            "-sALLOW_MEMORY_GROWTH=1",
            "-sDISABLE_EXCEPTION_CATCHING=0",
            "--no-entry",
            "-sEXPORTED_FUNCTIONS=_fs_ui_start,_fs_ui_show,_fs_ui_fire_event,_fs_ui_fire,_fs_ui_drop,_fs_ui_split,_fs_ui_vsplit,_fs_ui_key,_fs_ui_key_input,_fs_ui_cell_size,_fs_ui_file_pick,_fs_ui_scroll_pin,_fs_api_done,_fs_ui_backend_debug,_fs_ui_debug,_malloc,_free",
            "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8"
        }
    }
}