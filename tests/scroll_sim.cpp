// Runs the scrolling screenshot core against simulated apps and compares the long image with what the page really holds.
// Usage: ScrollSim [output folder] [scene name]. Writes one BMP per scene when given a folder.
#include "../src/config/core.h"
#include "../src/stitcher.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace sstk;

namespace {

struct Bitmap {
  int width = 0;
  int height = 0;
  std::vector<uint32_t> pixels;
  Bitmap() = default;
  Bitmap(int w, int h, uint32_t color = 0xFFFFFFFF) : width(w), height(h), pixels(static_cast<size_t>(w) * h, color) {}
  uint32_t& at(int x, int y) { return pixels[static_cast<size_t>(y) * width + x]; }
  uint32_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
  void Fill(int left, int top, int right, int bottom, uint32_t color) {
    for (int y = std::max(top, 0); y < std::min(bottom, height); ++y) {
      for (int x = std::max(left, 0); x < std::min(right, width); ++x) at(x, y) = color;
    }
  }
  PixelView view() const { return PixelView{reinterpret_cast<const uint8_t*>(pixels.data()), width, height, width * 4}; }
};

uint32_t Rgb(int r, int g, int b) {
  return 0xFF000000u | (static_cast<uint32_t>(std::clamp(r, 0, 255)) << 16) | (static_cast<uint32_t>(std::clamp(g, 0, 255)) << 8) | static_cast<uint32_t>(std::clamp(b, 0, 255));
}

uint32_t Tint(uint32_t c, int delta) {
  return Rgb(static_cast<int>((c >> 16) & 255) + delta, static_cast<int>((c >> 8) & 255) + delta, static_cast<int>(c & 255) + delta);
}

uint32_t Blend(uint32_t a, uint32_t b, int t, int total) {
  const auto mix = [&](int shift) { return (static_cast<int>((a >> shift) & 255) * (total - t) + static_cast<int>((b >> shift) & 255) * t) / total; };
  return Rgb(mix(16), mix(8), mix(0));
}

void Glyph(Bitmap& page, int x, int y, std::mt19937& rng, uint32_t ink, int height) {
  const int width = 4 + static_cast<int>(rng() % 4);
  const uint32_t pattern = rng();
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      if (((pattern >> ((row * 3 + col * 5) % 31)) & 1) != 0 && x + col < page.width && y + row < page.height) page.at(x + col, y + row) = ink;
    }
  }
}

int TextLine(Bitmap& page, int left, int right, int y, std::mt19937& rng, uint32_t ink, int size) {
  int x = left;
  const int end = left + static_cast<int>((right - left) * (0.45 + (rng() % 55) / 100.0));
  while (x < end) {
    const int word = 2 + static_cast<int>(rng() % 8);
    for (int i = 0; i < word && x < end; ++i) {
      Glyph(page, x, y, rng, ink, size);
      x += 6 + size / 4;
    }
    x += 6;
  }
  return y + size + 8;
}

// Paragraphs, headings, rules, cards, and pictures, every row of it unlike the others.
Bitmap MakePage(int width, int height, uint32_t seed, uint32_t background, uint32_t ink, bool cards = true) {
  Bitmap page(width, height, background);
  std::mt19937 rng(seed);
  int y = 24;
  const int left = 24;
  const int right = width - 24;
  while (y < height - 80) {
    const int kind = static_cast<int>(rng() % 10);
    if (kind == 0) {
      y = TextLine(page, left, right, y + 16, rng, ink, 18) + 8;
    } else if (kind == 1 && cards) {
      const int block = 90 + static_cast<int>(rng() % 160);
      page.Fill(left, y, right, y + block, Tint(background, -14));
      page.Fill(left, y, right, y + 1, Tint(background, -50));
      page.Fill(left, y + block - 1, right, y + block, Tint(background, -50));
      int inner = y + 14;
      while (inner < y + block - 22) inner = TextLine(page, left + 16, right - 16, inner, rng, ink, 11);
      y += block + 18;
    } else if (kind == 2 && cards) {
      const int block = 100 + static_cast<int>(rng() % 120);
      const int w = std::min(320 + static_cast<int>(rng() % 240), right - left);
      for (int yy = 0; yy < block; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
          const uint32_t n = (static_cast<uint32_t>(xx / 3) * 73856093u) ^ (static_cast<uint32_t>((y + yy) / 3) * 19349663u);
          if (y + yy < height) page.at(left + xx, y + yy) = Rgb(60 + static_cast<int>(n % 120), 80 + static_cast<int>((n >> 8) % 120), 100 + static_cast<int>((n >> 16) % 120));
        }
      }
      y += block + 18;
    } else if (kind == 3) {
      page.Fill(left, y + 6, right, y + 7, Tint(background, -40));
      y += 18;
    } else {
      const int lines = 1 + static_cast<int>(rng() % 5);
      for (int i = 0; i < lines; ++i) y = TextLine(page, left, right, y, rng, ink, 11);
      y += 10;
    }
  }
  return page;
}

struct Scene {
  std::string name;
  int window_w = 1200;
  int window_h = 800;
  RECT view{0, 40, 1200, 800};  // Where the page shows, in window pixels.
  float scale = 1.0f;
  int start = 0;
  std::vector<double> script;  // Page position per reading.
  std::vector<RECT> hints;
  int skip_top = 0;     // Rows of the image, from its top and bottom, that are not the page.
  int skip_bottom = 0;
  std::function<void(Bitmap&)> chrome;  // Draws what lies around the view.
  std::function<void(Bitmap&, const Bitmap&, double, POINT, int)> draw;  // Draws the view.
  int page_w = 1200;
  int page_h = 8000;
  int page_x = 0;  // Where the page's first column shows, in view pixels.
  POINT pointer{600, 400};
  uint32_t background = 0xFFFFFFFF;
  uint32_t ink = 0xFF202020;
  bool cards = true;
  int expect_left = -1;  // The columns the image should keep, in view pixels, if not the page's own.
  int expect_right = -1;
  double allow_bad = 0.0;  // Share of rows allowed to differ.
  int tolerance = 60;      // How far a pixel may differ, summed over the channels.
};

std::vector<double> Notches(int start, int notches, int notch, int steps, int rest_every = 4, int rest = 3) {
  std::vector<double> out;
  double at = start;
  for (int i = 0; i < 4; ++i) out.push_back(at);
  for (int n = 0; n < notches; ++n) {
    const double from = at;
    for (int s = 1; s <= steps; ++s) {
      const double t = static_cast<double>(s) / steps;
      out.push_back(std::round(from + notch * (1.0 - (1.0 - t) * (1.0 - t))));
    }
    at = from + notch;
    if (n % rest_every == rest_every - 1) {
      for (int r = 0; r < rest; ++r) out.push_back(at);
    }
  }
  for (int i = 0; i < 4; ++i) out.push_back(at);
  return out;
}

void Blit(Bitmap& to, const Bitmap& page, int page_x, int view_left, int view_top, int view_right, int view_bottom, int position) {
  for (int y = view_top; y < view_bottom; ++y) {
    const int row = y - view_top + position;
    if (row < 0 || row >= page.height) continue;
    const int width = std::min(view_right - view_left - page_x, page.width);
    std::memcpy(&to.at(view_left + page_x, y), &page.pixels[static_cast<size_t>(row) * page.width], static_cast<size_t>(width) * 4);
  }
}

void StaticPanel(Bitmap& to, int left, int top, int right, int bottom, uint32_t background, uint32_t ink, uint32_t seed) {
  to.Fill(left, top, right, bottom, background);
  std::mt19937 rng(seed);
  for (int y = top + 16; y < bottom - 24; y += 30) TextLine(to, left + 14, right - 14, y, rng, ink, 11);
}

struct Result {
  std::string name;
  bool locked = false;
  int width = 0;
  int height = 0;
  int expected_height = 0;
  int bad_rows = 0;
  int checked_rows = 0;
  int lost = 0;
  int misplaced = 0;
  int crop_left = 0;
  int crop_right = 0;
  int expected_left = 0;
  int expected_right = 0;
  double max_ms = 0.0;
  double mean_ms = 0.0;
  bool passed = false;
};

double Now() {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

void WriteBmp(const std::string& path, int width, int height, const uint8_t* data, int stride) {
  FILE* file = nullptr;
  if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) return;
  BITMAPFILEHEADER head{};
  BITMAPINFOHEADER info{};
  info.biSize = sizeof(info);
  info.biWidth = width;
  info.biHeight = -height;
  info.biPlanes = 1;
  info.biBitCount = 32;
  head.bfType = 0x4D42;
  head.bfOffBits = sizeof(head) + sizeof(info);
  head.bfSize = head.bfOffBits + static_cast<DWORD>(width) * height * 4;
  fwrite(&head, sizeof(head), 1, file);
  fwrite(&info, sizeof(info), 1, file);
  for (int y = 0; y < height; ++y) fwrite(data + static_cast<ptrdiff_t>(y) * stride, 4, width, file);
  fclose(file);
}

Result Run(const Scene& scene, const std::string& folder, bool only_summary) {
  Result result;
  result.name = scene.name;
  const Bitmap page = MakePage(scene.page_w, scene.page_h, std::hash<std::string>{}(scene.name) & 0xFFFF, scene.background, scene.ink, scene.cards);
  const int view_w = scene.view.right - scene.view.left;
  const int view_h = scene.view.bottom - scene.view.top;

  const auto render = [&](double position, POINT pointer, int reading) {
    Bitmap frame(scene.window_w, scene.window_h, 0xFFF0F0F0);
    if (scene.chrome) scene.chrome(frame);
    scene.draw(frame, page, position, pointer, reading);
    return frame;
  };

  EdgeFrame before;
  EdgeFrame after;
  Motion motion;
  MotionFinder finder;
  Stitcher stitcher;
  ScrollPlan plan;
  Bitmap previous = render(scene.script[0], scene.pointer, 0);
  before.Build(previous.view());
  const auto crop_view = [&](const Bitmap& frame) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(frame.pixels.data()) + static_cast<ptrdiff_t>(plan.rect.top) * frame.width * 4 + static_cast<ptrdiff_t>(plan.rect.left) * 4;
    return PixelView{data, plan.rect.right - plan.rect.left, plan.rect.bottom - plan.rect.top, frame.width * 4};
  };
  double total_ms = 0.0;
  int measured = 0;
  double first_position = scene.script[0];
  double low = first_position;
  double high = first_position;
  for (size_t i = 1; i < scene.script.size(); ++i) {
    const double position = scene.script[i];
    const Bitmap frame = render(position, scene.pointer, static_cast<int>(i));
    const double started = Now();
    if (!result.locked) {
      after.Build(frame.view());
      if (after.SameAs(before)) continue;
      finder.Analyze(before, after, 0, 0, motion);
      if (motion.moved) {
        plan = PlanScrollArea(before, after, motion, scene.pointer, true, scene.scale, scene.hints);
        if (plan.found) {
          first_position = scene.script[i - 1];
          low = high = first_position;
          const int plan_width = plan.rect.right - plan.rect.left;
          const int plan_height = plan.rect.bottom - plan.rect.top;
          const int limit = std::max(std::min(config::kScrollMaxHeight, config::kScrollMaxPixels / plan_width), plan_height);
          if (!stitcher.Start(crop_view(previous), scene.scale, limit, plan.hinted, plan.box)) break;
          result.locked = true;
        }
      }
      if (!result.locked) {
        std::swap(before, after);
        previous = frame;
        continue;
      }
    }
    low = std::min(low, position);
    high = std::max(high, position);
    const POINT local{scene.pointer.x - plan.rect.left, scene.pointer.y - plan.rect.top};
    const Stitcher::Step step = stitcher.Feed(crop_view(frame), local);
    if (step == Stitcher::Step::kLost) ++result.lost;
    const double ms = (Now() - started) * 1000.0;
    if (step != Stitcher::Step::kLost && std::abs(stitcher.position() - (position - first_position)) > 1.0) ++result.misplaced;
    total_ms += ms;
    result.max_ms = std::max(result.max_ms, ms);
    ++measured;
  }
  result.mean_ms = measured > 0 ? total_ms / measured : 0.0;
  if (!result.locked) return result;

  stitcher.Finish();
  const Stitcher::Crop crop = stitcher.FinalCrop();
  if (!only_summary) {
    printf("  plan %s rect %ld,%ld-%ld,%ld box %ld,%ld-%ld,%ld, crop %d-%d rows %d-%d, positions %d to %d\n", plan.hinted ? "hinted" : "free", plan.rect.left, plan.rect.top, plan.rect.right, plan.rect.bottom,
           plan.box.left, plan.box.top, plan.box.right, plan.box.bottom, crop.left, crop.right, crop.top, crop.bottom, stitcher.first_row(), stitcher.last_row() - stitcher.height());
  }
  result.width = crop.right - crop.left;
  result.height = crop.bottom - crop.top;
  std::vector<uint8_t> image(static_cast<size_t>(result.width) * result.height * 4);
  stitcher.Copy(crop, image.data(), static_cast<ptrdiff_t>(result.width) * 4);
  if (!folder.empty()) WriteBmp(folder + "\\" + scene.name + ".bmp", result.width, result.height, image.data(), result.width * 4);

  // Image rows against the page: canvas row r holds what window row r + rect.top showed at the first position.
  const int page_of_row0 = static_cast<int>(first_position) + plan.rect.top - scene.view.top + crop.top;
  const int left_in_view = crop.left + plan.rect.left - scene.view.left;
  result.crop_left = left_in_view;
  result.crop_right = left_in_view + result.width;
  const int expected_left = scene.expect_left >= 0 ? scene.expect_left : scene.page_x;
  const int expected_right = scene.expect_right >= 0 ? scene.expect_right : scene.page_x + std::min(view_w - scene.page_x, scene.page_w);
  result.expected_left = expected_left;
  result.expected_right = expected_right;
  result.expected_height = static_cast<int>(high - low) + view_h;
  for (int r = scene.skip_top; r < result.height - scene.skip_bottom; ++r) {
    const int row = page_of_row0 + r;
    if (row < 0 || row >= page.height) continue;
    int wrong = 0;
    int compared = 0;
    const uint32_t* line = reinterpret_cast<const uint32_t*>(image.data()) + static_cast<size_t>(r) * result.width;
    for (int x = 0; x < result.width; ++x) {
      const int page_x = x + left_in_view - scene.page_x;
      if (page_x < 0 || page_x >= page.width) continue;
      ++compared;
      const uint32_t want = page.at(page_x, row);
      const int d = std::abs(static_cast<int>(want & 255) - static_cast<int>(line[x] & 255)) + std::abs(static_cast<int>((want >> 8) & 255) - static_cast<int>((line[x] >> 8) & 255)) +
                    std::abs(static_cast<int>((want >> 16) & 255) - static_cast<int>((line[x] >> 16) & 255));
      if (d > scene.tolerance) ++wrong;
    }
    if (compared == 0) continue;
    ++result.checked_rows;
    if (wrong * 100 > compared * 2) ++result.bad_rows;
    if (wrong * 100 > compared * 2 && result.bad_rows % 60 == 1 && !only_summary) {
      for (int x = 0; x < result.width; ++x) {
        const int page_x = x + left_in_view - scene.page_x;
        if (page_x < 0 || page_x >= page.width || (line[x] & 0xFFFFFF) == (page.at(page_x, row) & 0xFFFFFF)) continue;
        printf("  row %d x %d got %06X want %06X (%d wrong)\n", r, x, line[x] & 0xFFFFFF, page.at(page_x, row) & 0xFFFFFF, wrong);
        break;
      }
    }
  }
  const bool size_ok = std::abs(result.crop_left - expected_left) <= 16 && std::abs(result.crop_right - expected_right) <= 16;
  const bool rows_ok = result.bad_rows <= static_cast<int>(result.checked_rows * scene.allow_bad);
  result.passed = size_ok && rows_ok && result.checked_rows > 0 && result.misplaced == 0;
  return result;
}

std::vector<Scene> Scenes() {
  std::vector<Scene> scenes;
  const auto base = [] {
    Scene scene;
    scene.chrome = [](Bitmap& frame) {
      frame.Fill(0, 0, frame.width, 40, 0xFFE4E4E4);
      std::mt19937 rng(7);
      TextLine(frame, 12, 300, 12, rng, 0xFF303030, 11);
      frame.Fill(0, 39, frame.width, 40, 0xFFB0B0B0);
    };
    scene.script = Notches(0, 30, 120, 5);
    scene.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) { Blit(frame, page, 0, 0, 40, frame.width, frame.height, static_cast<int>(position)); };
    return scene;
  };

  {
    Scene s = base();
    s.name = "plain";
    s.hints = {RECT{0, 40, 1200, 800}};
    scenes.push_back(s);
  }
  {
    // The page fills the window and nothing says where it is.
    Scene s = base();
    s.name = "no_hint";
    scenes.push_back(s);
  }
  {
    Scene s = base();
    s.name = "fast_and_back";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.script = Notches(1000, 12, 420, 2, 3, 2);
    const std::vector<double> back = Notches(static_cast<int>(s.script.back()), 12, -450, 3, 3, 2);
    s.script.insert(s.script.end(), back.begin(), back.end());
    scenes.push_back(s);
  }
  {
    Scene s = base();
    s.name = "sidebar_and_gutter";
    s.view = RECT{260, 40, 1200, 800};
    s.page_w = 920;
    s.expect_right = 920;
    s.pointer = POINT{700, 400};
    s.chrome = [](Bitmap& frame) {
      frame.Fill(0, 0, frame.width, 40, 0xFFE4E4E4);
      StaticPanel(frame, 0, 40, 260, frame.height, 0xFFF3F3F3, 0xFF404040, 3);
      frame.Fill(259, 40, 260, frame.height, 0xFFC8C8C8);
    };
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) {
      Blit(frame, page, 0, 260, 40, 1200, 800, static_cast<int>(position));
      frame.Fill(1180, 40, 1200, 800, 0xFFEEEEEE);
      const int thumb = 40 + static_cast<int>(position * 760.0 / page.height);
      frame.Fill(1184, thumb, 1196, thumb + 120, 0xFF9A9A9A);
    };
    scenes.push_back(s);
  }
  {
    Scene s = base();
    s.name = "sticky_fab_overlay_bar";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.skip_top = 56;
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) {
      Blit(frame, page, 0, 0, 40, 1200, 800, static_cast<int>(position));
      frame.Fill(0, 40, 1200, 96, 0xFF2B5FAB);
      std::mt19937 rng(11);
      TextLine(frame, 20, 500, 60, rng, 0xFFFFFFFF, 11);
      frame.Fill(1080, 690, 1170, 770, 0xFFD23F3F);
      frame.Fill(1100, 720, 1150, 740, 0xFFFFFFFF);
      const int thumb = 40 + static_cast<int>(position * 760.0 / page.height);
      for (int y = thumb; y < thumb + 100; ++y) {
        for (int x = 1190; x < 1198; ++x) frame.at(x, y) = Blend(frame.at(x, y), 0xFF505050, 2, 3);
      }
    };
    s.skip_bottom = 112;
    s.expect_right = 1188;
    s.allow_bad = 0.0;
    scenes.push_back(s);
  }
  {
    Scene s = base();
    s.name = "footer_appears_midway";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) {
      Blit(frame, page, 0, 0, 40, 1200, 800, static_cast<int>(position));
      if (position > 900) frame.Fill(0, 720, 1200, 800, 0xFF303A52);
      if (position > 900) frame.Fill(40, 750, 400, 770, 0xFFFFFFFF);
    };
    s.skip_bottom = 100;
    scenes.push_back(s);
  }
  {
    // Rows highlight under the pointer, and a tall card lights up whole.
    Scene s = base();
    s.name = "hover_rows";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.pointer = POINT{500, 420};
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT pointer, int) {
      const int at = static_cast<int>(position);
      Blit(frame, page, 0, 0, 40, 1200, 800, at);
      const int row_h = 44;
      const int row = (pointer.y - 40 + at) / row_h;
      const int top = row * row_h - at + 40;
      for (int y = std::max(top, 40); y < std::min(top + row_h, 800); ++y) {
        for (int x = 0; x < 1200; ++x) frame.at(x, y) = Tint(frame.at(x, y), -22);
      }
      frame.Fill(1100, top + 10, 1180, top + 34, 0xFF3060C0);
    };
    scenes.push_back(s);
  }
  {
    Scene s = base();
    s.name = "hover_near_bottom";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.pointer = POINT{500, 770};
    s.draw = scenes.back().draw;
    s.skip_bottom = 130;
    scenes.push_back(s);
  }
  {
    Scene s = base();
    s.name = "hover_tall_cards";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.pointer = POINT{500, 300};
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT pointer, int) {
      const int at = static_cast<int>(position);
      Blit(frame, page, 0, 0, 40, 1200, 800, at);
      const int card_h = 420;
      const int row = (pointer.y - 40 + at) / card_h;
      const int top = row * card_h - at + 40;
      for (int y = std::max(top, 40); y < std::min(top + card_h, 800); ++y) {
        for (int x = 0; x < 1200; ++x) frame.at(x, y) = Tint(frame.at(x, y), -18);
      }
    };
    s.allow_bad = 0.0;
    scenes.push_back(s);
  }
  {
    // Cards move over a patterned background that stays where it is.
    Scene s = base();
    s.name = "fixed_background";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.page_w = 760;
    s.page_x = 220;
    s.chrome = nullptr;
    s.pointer = POINT{600, 400};
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) {
      for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
          const uint32_t n = (static_cast<uint32_t>(x / 24) * 73856093u) ^ (static_cast<uint32_t>(y / 24) * 19349663u);
          frame.at(x, y) = Rgb(200 + static_cast<int>(n % 40), 205 + static_cast<int>((n >> 8) % 40), 210 + static_cast<int>((n >> 16) % 40));
        }
      }
      Blit(frame, page, 220, 0, 40, 1200, 800, static_cast<int>(position));
    };
    scenes.push_back(s);
  }
  {
    // Shades drift as content moves, and frames caught in motion sit between two pixel rows.
    Scene s = base();
    s.name = "see_through_blur";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.background = 0xFFECEFF4;
    std::vector<double> script;
    double at = 0;
    for (int i = 0; i < 4; ++i) script.push_back(at);
    for (int n = 0; n < 40; ++n) {
      for (int k = 0; k < 6; ++k) {
        at += 21.37;
        script.push_back(at);
      }
      at = std::round(at);
      script.push_back(at);
      script.push_back(at);
      script.push_back(at);
    }
    s.script = script;
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) {
      const int whole = static_cast<int>(std::floor(position));
      const int weight = static_cast<int>(std::lround((position - whole) * 16));
      for (int y = 40; y < 800; ++y) {
        const int a = y - 40 + whole;
        if (a < 0 || a + 1 >= page.height) continue;
        const int drift = static_cast<int>(std::lround(5.0 * std::sin(y / 97.0 + position / 211.0)));
        for (int x = 0; x < 1200; ++x) frame.at(x, y) = Tint(Blend(page.at(x, a), page.at(x, a + 1), weight, 16), drift);
      }
    };
    s.allow_bad = 0.35;  // Shades drift, and the last rows seen may be blurred. Their place is what counts here.
    s.tolerance = 200;
    scenes.push_back(s);
  }
  {
    Scene s = base();
    s.name = "video_in_page";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int reading) {
      const int at = static_cast<int>(position);
      Blit(frame, page, 0, 0, 40, 1200, 800, at);
      for (int row = 600; row < 1000; ++row) {
        const int y = row - at + 40;
        if (y < 40 || y >= 800) continue;
        for (int x = 200; x < 900; ++x) {
          const uint32_t n = (static_cast<uint32_t>(x / 4 + reading * 13) * 73856093u) ^ (static_cast<uint32_t>(row / 4 + reading * 7) * 19349663u);
          frame.at(x, y) = Rgb(static_cast<int>(n % 255), static_cast<int>((n >> 8) % 255), static_cast<int>((n >> 16) % 255));
        }
      }
    };
    s.allow_bad = 0.15;
    scenes.push_back(s);
  }
  {
    // A picture fills in a moment after it comes into view.
    Scene s = base();
    s.name = "lazy_image";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int reading) {
      const int at = static_cast<int>(position);
      Blit(frame, page, 0, 0, 40, 1200, 800, at);
      static int first_seen = -1;
      if (reading == 0) first_seen = -1;
      const int top = 1500 - at + 40;
      if (top < 800 && first_seen < 0) first_seen = reading;
      if (first_seen < 0 || reading < first_seen + 10) frame.Fill(100, top, 1100, top + 300, 0xFFE8E8E8);
    };
    scenes.push_back(s);
  }
  {
    // Nearly every row looks like the next.
    Scene s = base();
    s.name = "repeating_list";
    s.hints = {RECT{0, 40, 1200, 800}};
    s.cards = false;
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) {
      const int at = static_cast<int>(position);
      Blit(frame, page, 0, 0, 40, 1200, 800, at);
      for (int row = (at / 40) * 40; row < at + 800; row += 40) {
        const int y = row - at + 40;
        if (y >= 40 && y < 800) frame.Fill(0, y, 1200, y + 1, 0xFFC0C0C0);
      }
    };
    s.allow_bad = 1.0;
    scenes.push_back(s);
  }
  {
    // A pane in a page that stays put.
    Scene s = base();
    s.name = "nested_pane";
    s.view = RECT{400, 180, 1000, 660};
    s.page_w = 600;
    s.pointer = POINT{700, 400};
    s.script = Notches(0, 24, 100, 4);
    s.hints = {RECT{0, 40, 1200, 800}, RECT{400, 180, 1000, 660}};
    s.chrome = [](Bitmap& frame) {
      frame.Fill(0, 0, frame.width, 40, 0xFFE4E4E4);
      StaticPanel(frame, 0, 40, 1200, 800, 0xFFFFFFFF, 0xFF404040, 5);
      frame.Fill(398, 178, 1002, 662, 0xFFA0A0A0);
    };
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) { Blit(frame, page, 0, 400, 180, 1000, 660, static_cast<int>(position)); };
    scenes.push_back(s);
  }
  {
    // The same pane, with nothing to say where it is.
    Scene s = scenes.back();
    s.name = "nested_pane_no_hint";
    s.hints.clear();
    s.expect_left = 0;
    s.expect_right = 600;
    s.skip_bottom = 48;
    scenes.push_back(s);
  }
  {
    // A chat: a narrow column in wide margins, a sticky bar above, and a composer floating over the column.
    Scene s = base();
    s.name = "chat_layout";
    s.view = RECT{300, 40, 1500, 1000};
    s.window_w = 1600;
    s.window_h = 1000;
    s.page_w = 760;
    s.page_x = 220;
    s.expect_left = 0;
    s.expect_right = 1200;
    s.pointer = POINT{900, 500};
    s.hints = {RECT{300, 40, 1500, 1000}};
    s.background = 0xFF1F1F1F;
    s.ink = 0xFFE8E8E8;
    s.chrome = [](Bitmap& frame) {
      frame.Fill(0, 0, frame.width, 40, 0xFF181818);
      StaticPanel(frame, 0, 40, 300, frame.height, 0xFF141414, 0xFFBBBBBB, 9);
      frame.Fill(300, 40, frame.width, frame.height, 0xFF1F1F1F);
    };
    s.draw = [](Bitmap& frame, const Bitmap& page, double position, POINT, int) {
      Blit(frame, page, 220, 300, 40, 1500, 1000, static_cast<int>(position));
      frame.Fill(300, 40, 1500, 100, 0xFF262626);
      std::mt19937 rng(21);
      TextLine(frame, 340, 700, 62, rng, 0xFFFFFFFF, 11);
      frame.Fill(560, 860, 1240, 975, 0xFF2F2F2F);
      frame.Fill(561, 861, 1239, 974, 0xFF333333);
      std::mt19937 more(22);
      TextLine(frame, 590, 1100, 900, more, 0xFFCFCFCF, 11);
    };
    s.skip_top = 60;
    s.skip_bottom = 160;
    scenes.push_back(s);
  }
  {
    // Scrolled up from the middle, past where it started, under the sticky bar.
    Scene s = scenes.back();
    s.name = "chat_scrolled_up";
    s.script = Notches(3000, 24, -110, 5);
    s.skip_top = 120;  // What came out from under the bar last was seen only once.
    s.skip_bottom = 240;  // What lay under the composer when it started was never seen.
    scenes.push_back(s);
  }
  return scenes;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string folder = argc > 1 ? argv[1] : "";
  const std::string only = argc > 2 ? argv[2] : "";
  int failures = 0;
  printf("%-24s %-5s %6s %7s/%-7s %5s/%-5s %5s %5s %-11s %-11s %6s %6s\n", "scene", "pass", "width", "height", "expect", "bad", "rows", "lost", "off", "columns", "expected", "ms", "max");
  for (const Scene& scene : Scenes()) {
    if (!only.empty() && scene.name != only) continue;
    const Result r = Run(scene, folder, only.empty());
    printf("%-24s %-5s %6d %7d/%-7d %5d/%-5d %5d %5d %4d-%-6d %4d-%-6d %6.1f %6.1f\n", r.name.c_str(), r.passed ? "ok" : "FAIL", r.width, r.height, r.expected_height, r.bad_rows, r.checked_rows, r.lost,
           r.misplaced, r.crop_left, r.crop_right, r.expected_left, r.expected_right, r.mean_ms, r.max_ms);
    if (!r.passed) ++failures;
  }
  printf("%d failed\n", failures);
  return failures == 0 ? 0 : 1;
}
