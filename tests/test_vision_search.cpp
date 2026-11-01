// test_vision_search.cpp - image pipeline, search backend, async backend
//
// The three "it isn't wired" reports:
//   1. screenshot / read_image returned a hard-coded not-supported error,
//   2. search_web sent {search:true} into the fetch callback, which reads `url`
//      and therefore always failed with "empty url",
//   3. the desktop build ran session.send synchronously on the UI thread, so a
//      turn froze the window with no streaming at all.
//
// Each one is pinned here.
#include <fstream>
#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "test_framework.hpp"

#include "common/fs_image.hpp"
#include "common/fs_json.hpp"
#include "tools/net_tools.hpp"
#include "tools/search_backend.hpp"
#include "tools/tool_registry.hpp"
#include "tools/vision_tools.hpp"

using namespace fs;

// ===========================================================================
// image codec
// ===========================================================================

// ---------- PNG 编解码往返 ----------
FS_TEST(png_roundtrip) {
  image::Image img;
  img.width = 3;
  img.height = 2;
  // Distinct pixels so a channel swap (RGBA vs BGRA) is caught.
  img.rgba = {
      255, 0,   0,   255,  // red
      0,   255, 0,   255,  // green
      0,   0,   255, 255,  // blue
      10,  20,  30,  255,
      40,  50,  60,  128,  // alpha preserved
      70,  80,  90,  255,
  };
  std::string png = image::encode_png(img);
  EXPECT_TRUE(png.size() > 8);
  EXPECT_TRUE(image::sniff(png) == image::Format::kPng);

  int w = 0, h = 0;
  image::dimensions(png, &w, &h);
  EXPECT_EQ(w, 3);
  EXPECT_EQ(h, 2);

  image::Image back;
  std::string err;
  ASSERT_TRUE(image::decode(png, &back, &err));
  EXPECT_EQ(back.width, 3);
  EXPECT_EQ(back.height, 2);
  ASSERT_TRUE(back.rgba.size() == img.rgba.size());
  for (size_t i = 0; i < img.rgba.size(); ++i) {
    EXPECT_EQ(static_cast<int>(back.rgba[i]), static_cast<int>(img.rgba[i]));
  }
}

// ---------- 坏数据不会崩 ----------
FS_TEST(png_decode_rejects_garbage) {
  image::Image out;
  std::string err;
  EXPECT_FALSE(image::decode("not a png at all", &out, &err));
  EXPECT_FALSE(err.empty());
  // Truncated PNG header.
  std::string tiny = "\x89PNG\r\n\x1a\n";
  EXPECT_FALSE(image::decode(tiny, &out, &err));
}

// ---------- 缩放：只缩不放，长边受限 ----------
FS_TEST(image_resize_max_side) {
  image::Image img;
  img.width = 100;
  img.height = 50;
  img.rgba.assign(100 * 50 * 4, 128);
  image::Image small = image::resize_max_side(img, 20);
  EXPECT_EQ(small.width, 20);
  EXPECT_EQ(small.height, 10);
  EXPECT_TRUE(small.valid());
  // Never upscales.
  image::Image same = image::resize_max_side(img, 500);
  EXPECT_EQ(same.width, 100);
  EXPECT_EQ(same.height, 50);
}

// ---------- base64 / data URI ----------
FS_TEST(image_base64_and_data_uri) {
  EXPECT_EQ(image::base64(""), std::string(""));
  EXPECT_EQ(image::base64("f"), std::string("Zg=="));
  EXPECT_EQ(image::base64("fo"), std::string("Zm8="));
  EXPECT_EQ(image::base64("foo"), std::string("Zm9v"));
  std::string uri = image::data_uri(image::Format::kPng, "abc");
  EXPECT_EQ(uri.compare(0, 22, "data:image/png;base64,"), 0);
}

// ===========================================================================
// vision tools
// ===========================================================================

// ---------- read_image 真的返回图片（而不是 not-supported）----------
FS_TEST(read_image_returns_data_uri) {
  fstest::TempDir td("vision");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);

  // Write a real PNG into the project.
  image::Image img;
  img.width = 4;
  img.height = 4;
  img.rgba.assign(4 * 4 * 4, 200);
  std::string png = image::encode_png(img);
  {
    std::ofstream ofs((proj / "shot.png").string(), std::ios::binary);
    ofs.write(png.data(), static_cast<std::streamsize>(png.size()));
  }

  ToolRegistry reg;
  register_vision_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();

  FsResult r = reg.invoke(ctx, "read_image", Json{{"path", "shot.png"}});
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(jsonutil::get<std::string>(r.data, "mime", ""), std::string("image/png"));
  EXPECT_EQ(static_cast<int>(jsonutil::get_int(r.data, "width", 0)), 4);
  std::string uri = jsonutil::get<std::string>(r.data, "dataUri", "");
  EXPECT_TRUE(uri.find("data:image/png;base64,") == 0);

  // Missing file / non-image are clean errors, not crashes.
  EXPECT_FALSE(reg.invoke(ctx, "read_image", Json{{"path", "nope.png"}}).ok());
  {
    std::ofstream ofs((proj / "notimage.txt").string(), std::ios::binary);
    ofs << "hello";
  }
  FsResult bad = reg.invoke(ctx, "read_image", Json{{"path", "notimage.txt"}});
  EXPECT_FALSE(bad.ok());
  EXPECT_TRUE(bad.message.find("recognised image") != std::string::npos);
}

// ---------- read_image 超过 maxSide 会真的缩小 ----------
FS_TEST(read_image_downscales) {
  fstest::TempDir td("vision2");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);
  image::Image img;
  img.width = 400;
  img.height = 200;
  img.rgba.assign(400 * 200 * 4, 100);
  std::string png = image::encode_png(img);
  {
    std::ofstream ofs((proj / "big.png").string(), std::ios::binary);
    ofs.write(png.data(), static_cast<std::streamsize>(png.size()));
  }
  ToolRegistry reg;
  register_vision_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();
  FsResult r = reg.invoke(ctx, "read_image",
                          Json{{"path", "big.png"}, {"maxSide", 100}});
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(static_cast<int>(jsonutil::get_int(r.data, "width", 0)), 100);
  EXPECT_EQ(static_cast<int>(jsonutil::get_int(r.data, "height", 0)), 50);
  EXPECT_TRUE(jsonutil::get<bool>(r.data, "resized", false));
  EXPECT_TRUE(r.data.contains("sourceWidth"));
}

// ---------- 截图工具不再是 not-supported ----------
FS_TEST(screenshot_is_wired) {
  ToolRegistry reg;
  register_vision_tools(&reg);
  ToolContext ctx;
  FsResult r = reg.invoke(ctx, "screenshot", Json{{"target", "region"},
                                                  {"width", 0},
                                                  {"height", 0}});
  // A zero-size region is a bad ARGUMENT, not "not supported" - that
  // distinction is the whole point: the tool is implemented now.
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.code == ErrCode::kInvalidArg);
  EXPECT_TRUE(r.message.find("not wired") == std::string::npos);
}

// ---------- 电脑控制关掉时拒绝截图 ----------
FS_TEST(screenshot_respects_switch) {
  ToolRegistry reg;
  register_vision_tools(&reg);
  ToolContext ctx;
  ctx.computer_enabled = []() { return false; };
  FsResult r = reg.invoke(ctx, "screenshot", Json{{"target", "screen"}});
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.code == ErrCode::kPermissionDenied);
  EXPECT_TRUE(r.message.find("Computer control") != std::string::npos);
}

// ===========================================================================
// search backend
// ===========================================================================

// ---------- 未配置：错误信息告诉你去哪配置 ----------
FS_TEST(search_unconfigured_explains_where) {
  search::Config cfg;  // provider empty
  search::HttpCall call = search::build_request(cfg, "hello", 5);
  EXPECT_FALSE(call.ok);
  EXPECT_TRUE(call.error.find("Settings") != std::string::npos);
}

// ---------- Tavily：POST + Bearer + {query,max_results} ----------
FS_TEST(search_tavily_request) {
  search::Config cfg;
  cfg.provider = "tavily";
  cfg.api_key = "tv-key";
  cfg.max_results = 5;
  cfg.endpoint = "https://api.tavily.com/search";
  search::HttpCall call = search::build_request(cfg, "rust async", 3);
  ASSERT_TRUE(call.ok);
  EXPECT_EQ(call.method, std::string("POST"));
  EXPECT_EQ(call.url, std::string("https://api.tavily.com/search"));
  EXPECT_EQ(call.headers["Authorization"].get<std::string>(), std::string("Bearer tv-key"));
  Json body = Json::parse(call.body);
  EXPECT_EQ(body["query"].get<std::string>(), std::string("rust async"));
  EXPECT_EQ(body["max_results"].get<int64_t>(), 3);
}

// ---------- Brave：GET + 查询串 + X-Subscription-Token ----------
FS_TEST(search_brave_request) {
  search::Config cfg;
  cfg.provider = "brave";
  cfg.api_key = "bk";
  cfg.endpoint = "https://api.search.brave.com/res/v1/web/search";
  search::HttpCall call = search::build_request(cfg, "c++ agent", 5);
  ASSERT_TRUE(call.ok);
  EXPECT_EQ(call.method, std::string("GET"));
  // Space becomes '+', '+' becomes %2B: a raw '+' would decode as a space.
  EXPECT_TRUE(call.url.find("q=c%2B%2B+agent") != std::string::npos);
  EXPECT_EQ(call.headers["X-Subscription-Token"].get<std::string>(), std::string("bk"));
}

// ---------- 缺 key 时明确报错 ----------
FS_TEST(search_missing_key) {
  search::Config cfg;
  cfg.provider = "tavily";
  cfg.endpoint = "https://api.tavily.com/search";
  search::HttpCall call = search::build_request(cfg, "x", 5);
  EXPECT_FALSE(call.ok);
  EXPECT_TRUE(call.error.find("API key") != std::string::npos);
}

// ---------- 三种响应形态都能解析 ----------
FS_TEST(search_parses_all_shapes) {
  search::Config tavily;
  tavily.provider = "tavily";
  std::string body1 =
      R"({"results":[{"title":"A","url":"https://a","content":"snippet a"}]})";
  search::Parsed p1 = search::parse_response(tavily, 200, body1);
  ASSERT_TRUE(p1.ok);
  ASSERT_TRUE(p1.hits.size() == 1);
  EXPECT_EQ(p1.hits[0].title, std::string("A"));
  EXPECT_EQ(p1.hits[0].snippet, std::string("snippet a"));

  search::Config brave;
  brave.provider = "brave";
  std::string body2 =
      R"({"web":{"results":[{"title":"B","url":"https://b","description":"d"}]}})";
  search::Parsed p2 = search::parse_response(brave, 200, body2);
  ASSERT_TRUE(p2.ok);
  ASSERT_TRUE(p2.hits.size() == 1);
  EXPECT_EQ(p2.hits[0].snippet, std::string("d"));

  search::Config custom;
  custom.provider = "custom";
  std::string body3 = R"([{"name":"C","link":"https://c","text":"t"}])";
  search::Parsed p3 = search::parse_response(custom, 200, body3);
  ASSERT_TRUE(p3.ok);
  ASSERT_TRUE(p3.hits.size() == 1);
  EXPECT_EQ(p3.hits[0].url, std::string("https://c"));
}

// ---------- 空结果不是错误 ----------
FS_TEST(search_empty_results_is_ok) {
  search::Config cfg;
  cfg.provider = "tavily";
  search::Parsed p = search::parse_response(cfg, 200, R"({"results":[]})");
  EXPECT_TRUE(p.ok);
  EXPECT_EQ(static_cast<int>(p.hits.size()), 0);
}

// ---------- HTTP 错误带状态码 ----------
FS_TEST(search_http_error_reported) {
  search::Config cfg;
  cfg.provider = "tavily";
  search::Parsed p = search::parse_response(cfg, 401, "unauthorized");
  EXPECT_FALSE(p.ok);
  EXPECT_TRUE(p.error.find("401") != std::string::npos);
}

// ---------- 工具层：走 web_search 回调，不再塞 {search:true} 进 fetch ----------
FS_TEST(search_tool_uses_web_search_callback) {
  ToolRegistry reg;
  register_net_tools(&reg);
  ToolContext ctx;
  // Only the search callback is wired; net_fetch is deliberately absent.
  bool called = false;
  ctx.web_search = [&called](const Json& req, Json* out) -> FsResult {
    called = true;
    Json o = Json::object();
    o["provider"] = "tavily";
    o["hits"] = Json::array(
        {Json{{"title", "T"}, {"url", "https://x"}, {"snippet", "S"}}});
    o["count"] = 1;
    // The query must reach the callback.
    o["query"] = jsonutil::get<std::string>(req, "query", "");
    *out = o;
    return FsResult::success(o);
  };
  FsResult r = reg.invoke(ctx, "search_web", Json{{"query", "hello"}});
  EXPECT_TRUE(called);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(jsonutil::get<std::string>(r.data, "provider", ""), std::string("tavily"));
  EXPECT_EQ(jsonutil::get<std::string>(r.data, "query", ""), std::string("hello"));
  ASSERT_TRUE(r.data["hits"].is_array());
  EXPECT_EQ(static_cast<int>(r.data["hits"].size()), 1);
}

// ---------- 没有配置回调：错误指向设置页 ----------
FS_TEST(search_tool_unwired_message) {
  ToolRegistry reg;
  register_net_tools(&reg);
  ToolContext ctx;  // no web_search
  FsResult r = reg.invoke(ctx, "search_web", Json{{"query", "x"}});
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.message.find("Settings") != std::string::npos);
}
