#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cmath>
#include <random>

#include <ft2build.h>
#include FT_FREETYPE_H

// ПОДКЛЮЧАЕМ HARFBUZZ
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include <cmath>   // Для std::exp
#include <cstdlib> // Для std::rand
#include <ctime>   // Для std::time

#include <iostream>
#include <vector>
#include <string>
#include <limits>
#include <algorithm>
#include <fstream>
#include <map>

#define NOMINMAX 
#include <windows.h>
#include <cstdint>

// =====================================================================
// КЛАСС ДЛЯ ОТРИСОВКИ ОКНА
// =====================================================================
class DebugWindow {
    HWND hwnd;
    HBITMAP hBitmap;
    HDC hdcMem;
    void* pixels;
    int width, height;

public:
    DebugWindow(int w, int h) : width(w), height(h) {
        WNDCLASS wc = { 0 };
        wc.lpfnWndProc = DefWindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"ZalgoDebugWindow";
        RegisterClass(&wc);

        hwnd = CreateWindowEx(0, L"ZalgoDebugWindow", L"Real-time Render Tracker",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, w + 20, h + 40,
            NULL, NULL, wc.hInstance, NULL);

        HDC hdc = GetDC(hwnd);
        hdcMem = CreateCompatibleDC(hdc);

        BITMAPINFO bmi = { 0 };
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pixels, NULL, 0);
        SelectObject(hdcMem, hBitmap);
        ReleaseDC(hwnd, hdc);

        memset(pixels, 0, w * h * 4);
    }

    ~DebugWindow() {
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        DestroyWindow(hwnd);
    }

    void UpdateFromCanvas(const std::vector<float>& global_canvas) {
        uint32_t* bmpPixels = (uint32_t*)pixels;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                // При отрисовке применяем clamp до 255
                uint8_t val = (uint8_t)std::min(255.0f, global_canvas[y * width + x]);
                bmpPixels[y * width + x] = (val << 16) | (val << 8) | val | 0xFF000000;
            }
        }
    }

    void Draw() {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        HDC hdc = GetDC(hwnd);
        BitBlt(hdc, 0, 0, width, height, hdcMem, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, hdc);
    }
};

void CopyUtf8ToClipboard(const std::string& utf8Text) {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), -1, NULL, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), -1, &wstr[0], size_needed);
    OpenClipboard(nullptr);
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wstr.size()) * sizeof(wchar_t));
    if (hMem) {
        memcpy(GlobalLock(hMem), wstr.c_str(), wstr.size() * sizeof(wchar_t));
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
}

std::string utf8_encode(char32_t codepoint) {
    std::string out;
    if (codepoint <= 0x7F) { out += static_cast<char>(codepoint); }
    else if (codepoint <= 0x7FF) { out += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)); out += static_cast<char>(0x80 | (codepoint & 0x3F)); }
    else if (codepoint <= 0xFFFF) { out += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)); out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)); out += static_cast<char>(0x80 | (codepoint & 0x3F)); }
    else { out += static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)); out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)); out += static_cast<char>(0x80 | (codepoint & 0x3F)); }
    return out;
}

// =====================================================================
// СТРУКТУРЫ ДЛЯ МНОГОПРОХОДНОГО РЕНДЕРА
// =====================================================================
struct ClusterRender {
    std::vector<float> pixels;
    int w = 0, h = 0;
    int offset_x = 0, offset_y = 0;
    int advance_x = 0;
};

struct BaseGlyphInfo {
    char32_t code = 0;
    ClusterRender render;
};

// Хранит поставленный символ и его состояние для мутаций
struct PlacedCluster {
    char32_t base = 0;
    std::vector<char32_t> mods;
    int x = 0, y = 0;
    ClusterRender render;
    int base_advance = 0; // Ширина чистой базы (без модификаторов)
    bool is_newline = false;
};

ClusterRender RenderCluster(FT_Face face, hb_font_t* hb_font, char32_t base, const std::vector<char32_t>& mods) {
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add(buf, base, 0);
    for (char32_t m : mods) hb_buffer_add(buf, m, 0);

    hb_buffer_guess_segment_properties(buf);
    hb_shape(hb_font, buf, NULL, 0);

    unsigned int len = hb_buffer_get_length(buf);
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, NULL);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, NULL);

    struct PlacedGlyph { int x, y, w, h; std::vector<float> px; };
    std::vector<PlacedGlyph> placed;

    int pen_x = 0, pen_y = 0;
    int min_x = 999999, min_y = 999999, max_x = -999999, max_y = -999999;

    for (unsigned int i = 0; i < len; ++i) {
        FT_Load_Glyph(face, info[i].codepoint, FT_LOAD_RENDER);
        FT_Bitmap& bmp = face->glyph->bitmap;

        int gx = pen_x + (pos[i].x_offset >> 6) + face->glyph->bitmap_left;
        int gy = pen_y - (pos[i].y_offset >> 6) - face->glyph->bitmap_top;

        if (bmp.width > 0 && bmp.rows > 0) {
            min_x = std::min(min_x, gx);
            min_y = std::min(min_y, gy);
            max_x = std::max(max_x, gx + (int)bmp.width);
            max_y = std::max(max_y, gy + (int)bmp.rows);

            PlacedGlyph pg;
            pg.x = gx; pg.y = gy; pg.w = bmp.width; pg.h = bmp.rows;
            pg.px.assign(bmp.buffer, bmp.buffer + (bmp.width * bmp.rows));
            placed.push_back(pg);
        }
        pen_x += pos[i].x_advance >> 6;
        pen_y -= pos[i].y_advance >> 6;
    }

    ClusterRender out;
    out.advance_x = pen_x;

    if (placed.empty()) {
        out.w = out.h = 0;
        hb_buffer_destroy(buf);
        return out;
    }

    out.offset_x = min_x;
    out.offset_y = min_y;
    out.w = max_x - min_x;
    out.h = max_y - min_y;
    out.pixels.assign(out.w * out.h, 0.0f);

    for (const auto& pg : placed) {
        for (int y = 0; y < pg.h; ++y) {
            for (int x = 0; x < pg.w; ++x) {
                int px = (pg.x - min_x) + x;
                int py = (pg.y - min_y) + y;
                float val = pg.px[y * pg.w + x];
                out.pixels[py * out.w + px] = std::min(255.0f, out.pixels[py * out.w + px] + val);
            }
        }
    }

    hb_buffer_destroy(buf);
    return out;
}

// Добавляем или убираем пиксели кластера из холста
void ApplyCluster(std::vector<float>& canvas, const PlacedCluster& pc, int width, int height, int ascender, bool add) {
    if (pc.is_newline) return;
    int abs_start_x = pc.x + pc.render.offset_x;
    int abs_start_y = pc.y + ascender + pc.render.offset_y;

    for (int y = 0; y < pc.render.h; ++y) {
        for (int x = 0; x < pc.render.w; ++x) {
            int px = abs_start_x + x;
            int py = abs_start_y + y;
            if (px >= 0 && px < width && py >= 0 && py < height) {
                float val = pc.render.pixels[y * pc.render.w + x];
                if (add) {
                    canvas[py * width + px] += val;
                }
                else {
                    canvas[py * width + px] -= val;
                    if (canvas[py * width + px] < 0.001f) canvas[py * width + px] = 0.0f; // Защита от потери точности
                }
            }
        }
    }
}

float CalculateImprovement(const std::vector<unsigned char>& img, int img_w, int img_h,
    const std::vector<float>& global_canvas, // сырая сумма
    int cursor_x, int cursor_y, int ascender, int line_height,
    const ClusterRender& cluster) {

    if (cluster.h > line_height * 2.5) {
        return -999999.0f;
    }

    float improvement = 0.0f;
    int abs_start_x = cursor_x + cluster.offset_x;
    int abs_start_y = cursor_y + ascender + cluster.offset_y;

    for (int y = 0; y < cluster.h; ++y) {
        for (int x = 0; x < cluster.w; ++x) {
            int img_px = abs_start_x + x;
            int img_py = abs_start_y + y;
            float cluster_val = cluster.pixels[y * cluster.w + x];

            if (img_px < 0 || img_px >= img_w || img_py < 0 || img_py >= img_h) {
                if (cluster_val > 5.0f) {
                    improvement -= cluster_val * 2.0f;
                }
                continue;
            }

            float target_val = img[img_py * img_w + img_px];

            // ВАЖНО: Обрезаем значения только при вычислении визуальной разницы
            float old_val_clamped = std::min(255.0f, global_canvas[img_py * img_w + img_px]);
            float new_val_clamped = std::min(255.0f, global_canvas[img_py * img_w + img_px] + cluster_val);

            float e_old = (target_val - old_val_clamped) * (target_val - old_val_clamped);
            float e_new = (target_val - new_val_clamped) * (target_val - new_val_clamped);

            improvement += (e_old - e_new);
        }
    }
    return improvement;
}

int main() {
    setlocale(LC_ALL, "");

    std::string font_path = "C:\\Windows\\Fonts\\segoeui.ttf";

    int width, height, channels;
    unsigned char* data = stbi_load("Official_portrait_of_Barack_Obama.png", &width, &height, &channels, 3);
    if (!data) { std::cerr << "Не удалось загрузить картинку!\n"; return -1; }

    std::vector<unsigned char> gray_img(width * height);
    for (int i = 0; i < width * height; ++i) {
        gray_img[i] = 0.299 * data[i * 3] + 0.587 * data[i * 3 + 1] + 0.114 * data[i * 3 + 2];
    }
    stbi_image_free(data);

    // Холст хранит "сырую" сумму пикселей
    std::vector<float> global_canvas(width * height, 0.0f);
    DebugWindow debugWin(width, height);

    FT_Library ft;
    FT_Init_FreeType(&ft);

    FT_Face face;
    if (FT_New_Face(ft, font_path.c_str(), 0, &face)) {
        std::cerr << "Не удалось загрузить шрифт!\n"; return -1;
    }

    hb_font_t* hb_font = hb_ft_font_create(face, NULL);

    int font_size = 101;
    FT_Set_Pixel_Sizes(face, 0, font_size);

    int line_height = face->size->metrics.height >> 6;
    int ascender = face->size->metrics.ascender >> 6;

    // СТРУКТУРЫ ДЛЯ КЭША
    std::vector<BaseGlyphInfo> all_bases;
    std::map<int, std::vector<BaseGlyphInfo>> bases_by_advance;
    std::vector<char32_t> combining_chars;

    std::cout << "Сбор и пререндер символов...\n";
    FT_ULong charcode;
    FT_UInt gindex;
    charcode = FT_Get_First_Char(face, &gindex);
    while (gindex != 0) {
        bool is_rtl_block = (charcode >= 0x0590 && charcode <= 0x08FF) || (charcode >= 0xFB50 && charcode <= 0xFDFF) || (charcode >= 0xFE70 && charcode <= 0xFEFC);
        bool is_invisible_or_format = (charcode == 0x00AD) || (charcode == 0x034F) || (charcode >= 0x200B && charcode <= 0x200F) || (charcode >= 0x202A && charcode <= 0x202E) || (charcode >= 0x2060 && charcode <= 0x206F) || (charcode >= 0xFE00 && charcode <= 0xFE0F) || (charcode == 0xFEFF) || (charcode == 0x25AA);
        bool is_system_control = (charcode < 32) || (charcode >= 127 && charcode <= 159);
        bool is_broken = (charcode == 0xFDFD || charcode == 0x2E3B );

        if (!is_rtl_block && !is_invisible_or_format && !is_system_control && !is_broken) {
            if (FT_Load_Glyph(face, gindex, FT_LOAD_DEFAULT) == 0) {
                double max_advance_threshold = 0.4;

                if (face->glyph->advance.x == 0 && face->glyph->metrics.width > 0) {
                    combining_chars.push_back(charcode);
                }
                else if (face->glyph->advance.x > 0 && (face->glyph->advance.x >> 6) < (font_size * max_advance_threshold)) {
                    ClusterRender cluster = RenderCluster(face, hb_font, charcode, {});
                    if (cluster.advance_x > 0) {
                        BaseGlyphInfo info = { charcode, cluster };
                        all_bases.push_back(info);
                        bases_by_advance[cluster.advance_x].push_back(info);
                    }
                }
            }
        }
        charcode = FT_Get_Next_Char(face, charcode, &gindex);
    }

    std::cout << "Этап 1: Первичное покрытие (Жадный алгоритм)...\n";

    int cursor_x = 0;
    int cursor_y = 0;
    const int MAX_LAYERS = 6;

    // Хранит всю сетку символов
    std::vector<PlacedCluster> document;

    int debug_counter = 0;

    while (cursor_y + line_height <= height) {
        float max_improvement_per_px = -std::numeric_limits<float>::max();
        char32_t best_base = 0;
        ClusterRender best_base_cluster;

        // Поиск базы по кэшу
        for (const auto& bgi : all_bases) {
            float imp = CalculateImprovement(gray_img, width, height, global_canvas, cursor_x, cursor_y, ascender, line_height, bgi.render);
            float imp_per_w = imp / bgi.render.advance_x;

            if (imp_per_w > max_improvement_per_px) {
                max_improvement_per_px = imp_per_w;
                best_base = bgi.code;
                best_base_cluster = bgi.render;
            }
        }

        if (best_base != 0) {
            std::vector<char32_t> current_mods;
            ClusterRender current_cluster = best_base_cluster;
            float current_improvement = CalculateImprovement(gray_img, width, height, global_canvas, cursor_x, cursor_y, ascender, line_height, current_cluster);

            for (int layer = 0; layer < MAX_LAYERS; ++layer) {
                float best_mod_improvement = current_improvement;
                char32_t best_modifier = 0;
                ClusterRender best_mod_cluster;

                for (char32_t mod : combining_chars) {
                    if (std::find(current_mods.begin(), current_mods.end(), mod) != current_mods.end()) continue;

                    std::vector<char32_t> test_mods = current_mods;
                    test_mods.push_back(mod);

                    ClusterRender cluster = RenderCluster(face, hb_font, best_base, test_mods);
                    float imp = CalculateImprovement(gray_img, width, height, global_canvas, cursor_x, cursor_y, ascender, line_height, cluster);

                    if (imp > best_mod_improvement) {
                        best_mod_improvement = imp;
                        best_modifier = mod;
                        best_mod_cluster = cluster;
                    }
                }

                if (best_modifier != 0) {
                    current_improvement = best_mod_improvement;
                    current_mods.push_back(best_modifier);
                    current_cluster = best_mod_cluster;
                }
                else {
                    break;
                }
            }

            PlacedCluster pc;
            pc.base = best_base;
            pc.mods = current_mods;
            pc.x = cursor_x;
            pc.y = cursor_y;
            pc.render = current_cluster;
            pc.base_advance = best_base_cluster.advance_x; // Запоминаем изначальную ширину базы

            document.push_back(pc);
            ApplyCluster(global_canvas, pc, width, height, ascender, true);

            cursor_x += current_cluster.advance_x;

            if (++debug_counter % 50 == 0) {
                debugWin.UpdateFromCanvas(global_canvas);
                debugWin.Draw();
            }
        }
        else {
            cursor_x += 10;
        }

        if (cursor_x >= width) {
            PlacedCluster nl;
            nl.is_newline = true;
            document.push_back(nl);

            cursor_x = 0;
            cursor_y += line_height;
        }
    }

    std::cout << "Этап 2: Многопроходная оптимизация (Мутации)...\n";

    const int NUM_PASSES = 5; // Количество проходов (можно увеличить для лучшего качества)

    float initial_temp = 50000.0f;

    for (int pass = 0; pass < NUM_PASSES; ++pass) {
        std::cout << "  Проход " << (pass + 1) << " из " << NUM_PASSES << "...\n";
        float temp = initial_temp / (pass + 1.0f); // Температура падает с каждым проходом

        for (auto& pc : document) {
            if (pc.is_newline) continue;

            // 1. УДАЛЯЕМ текущий символ с холста
            ApplyCluster(global_canvas, pc, width, height, ascender, false);

            // 2. ИЩЕМ НОВЫЙ БАЗОВЫЙ СИМВОЛ
            // Создаем безопасную стартовую точку (рендерим изначальную чистую базу)
            ClusterRender best_base_cluster = RenderCluster(face, hb_font, pc.base, {});
            float best_imp = CalculateImprovement(gray_img, width, height, global_canvas, pc.x, pc.y, ascender, line_height, best_base_cluster);
            char32_t best_base = pc.base;

            // Ищем замену только среди символов с такой же оригинальной шириной (base_advance)
            for (const auto& bgi : bases_by_advance[pc.base_advance]) {
                float imp = CalculateImprovement(gray_img, width, height, global_canvas, pc.x, pc.y, ascender, line_height, bgi.render);
                if (imp > best_imp) {
                    best_imp = imp;
                    best_base = bgi.code;
                    best_base_cluster = bgi.render;
                }
            }

            // 3. ЗАНОВО ПОДБИРАЕМ МОДИФИКАТОРЫ (для новой/старой базы)
            std::vector<char32_t> current_mods;
            ClusterRender current_cluster = best_base_cluster;
            float current_improvement = CalculateImprovement(gray_img, width, height, global_canvas, pc.x, pc.y, ascender, line_height, current_cluster);

            for (int layer = 0; layer < MAX_LAYERS; ++layer) {
                float best_mod_improvement = current_improvement;
                char32_t best_modifier = 0;
                ClusterRender best_mod_cluster;

                for (char32_t mod : combining_chars) {
                    if (std::find(current_mods.begin(), current_mods.end(), mod) != current_mods.end()) continue;

                    std::vector<char32_t> test_mods = current_mods;
                    test_mods.push_back(mod);

                    ClusterRender cluster = RenderCluster(face, hb_font, best_base, test_mods);
                    float imp = CalculateImprovement(gray_img, width, height, global_canvas, pc.x, pc.y, ascender, line_height, cluster);

                    if (imp > best_mod_improvement) {
                        best_mod_improvement = imp;
                        best_modifier = mod;
                        best_mod_cluster = cluster;
                    }
                }

                if (best_modifier != 0) {
                    current_improvement = best_mod_improvement;
                    current_mods.push_back(best_modifier);
                    current_cluster = best_mod_cluster;
                }
                else {
                    break;
                }
            }

            // 4. СОХРАНЯЕМ МУТАЦИЮ и ВОЗВРАЩАЕМ НА ХОЛСТ
            pc.base = best_base;
            pc.mods = current_mods;
            pc.render = current_cluster;
            // pc.base_advance остается прежним, так как ширина базы гарантированно совпадает

            ApplyCluster(global_canvas, pc, width, height, ascender, true);

            if (++debug_counter % 100 == 0) {
                debugWin.UpdateFromCanvas(global_canvas);
                debugWin.Draw();
            }
        }
    }

    std::cout << "Сборка финального текста...\n";
    std::string result_art = "";
    for (const auto& pc : document) {
        if (pc.is_newline) {
            result_art += '\n';
        }
        else {
            // Экранирование для HTML-отображения (если вставляете в браузер)
            if (pc.base == '<') result_art += "&lt;";
            else if (pc.base == '>') result_art += "&gt;";
            else if (pc.base == '&') result_art += "&amp;";
            else result_art += utf8_encode(pc.base);

            for (char32_t mod : pc.mods) {
                result_art += utf8_encode(mod);
            }
        }
    }

    CopyUtf8ToClipboard(result_art);
    std::cout << "Готово! Сохранено в буфер обмена.\n";

    hb_font_destroy(hb_font);
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    return 0;
}