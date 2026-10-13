// ui_api.hpp - HTTP API dispatch: method name -> FsApi call.
//
// The wasm UI runs in a browser and only links src/ui + src/common, so it
// cannot call fs_core in-process. For the web build the native exe serves it
// over HTTP (rules/addon3.md: "网页版：开启静态网页+API服务"):
//
//   POST /api/call   {"method":"session.send","args":{...}}
//                 -> {"ok":true,"data":{...}}
//                 -> {"ok":false,"code":"not-found","message":"..."}
//   GET  /api/events?since=N  -> {"events":[{id,event,payload}],"next":M}
//
// The WhaleUI desktop build will instead use a NativeBackend calling FsApi
// directly in-process, so UiApp only ever sees IUiBackend.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

class FsApi;

namespace ui {

class UiApi {
 public:
  explicit UiApi(FsApi* api);

  // Dispatch a method; unknown method -> kNotFound.
  FsResult call(const std::string& method, const Json& args);
  bool has(const std::string& method) const;

  // Registered method names (diagnostics / tests).
  std::vector<std::string> methods() const;

 private:
  void reg(const std::string& name, std::function<FsResult(const Json&)> fn);
  std::map<std::string, std::function<FsResult(const Json&)> > methods_;
};

}  // namespace ui
}  // namespace fs
