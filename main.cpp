#include "SystemInfo.h"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr Color kBackground{18, 20, 25, 255};
constexpr Color kSurface{27, 30, 37, 255};
constexpr Color kSurfaceRaised{34, 38, 46, 255};
constexpr Color kBorder{49, 54, 64, 255};
constexpr Color kText{235, 238, 244, 255};
constexpr Color kMuted{148, 156, 170, 255};
constexpr Color kBlue{92, 156, 255, 255};
constexpr Color kGreen{78, 199, 145, 255};

std::string formatBytes(std::uint64_t bytes) {
    constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream result;
    result << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' ' << units[unit];
    return result.str();
}

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return static_cast<char>(character);
    });
    return text;
}

bool matchesQuery(const ProcessInfo& process, const std::string& query) {
    if (query.empty()) {
        return true;
    }
    const std::string normalizedQuery = lowerAscii(query);
    const std::string normalizedName = lowerAscii(process.name);
    const std::string pid = std::to_string(process.pid);
    return normalizedName.find(normalizedQuery) != std::string::npos
        || pid.find(normalizedQuery) != std::string::npos;
}

void removeLastUtf8Character(std::string& value) {
    if (value.empty()) {
        return;
    }
    value.pop_back();
    while (!value.empty()
           && (static_cast<unsigned char>(value.back()) & 0xC0U) == 0x80U) {
        value.pop_back();
    }
}

void appendUtf8(std::string& value, int codepoint) {
    if (codepoint <= 0x7F) {
        value.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        value.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        value.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        value.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        value.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        value.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        value.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        value.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string truncateText(const std::string& text, float maxWidth, float fontSize) {
    std::string shortened = text;
    constexpr const char* suffix = "...";
    while (!shortened.empty()
           && MeasureTextEx(GetFontDefault(), (shortened + suffix).c_str(), fontSize, 1.0f).x > maxWidth) {
        removeLastUtf8Character(shortened);
    }
    return shortened == text ? text : shortened + suffix;
}

void drawTextRight(const std::string& text, float right, float y, float fontSize, Color color) {
    const Vector2 size = MeasureTextEx(GetFontDefault(), text.c_str(), fontSize, 1.0f);
    DrawTextEx(GetFontDefault(), text.c_str(), {right - size.x, y}, fontSize, 1.0f, color);
}

void drawMetricCard(Rectangle bounds, const char* title, const std::string& value,
                    const std::string& detail, double percentage, Color accent) {
    DrawRectangleRounded(bounds, 0.12f, 10, kSurface);
    DrawRectangleRoundedLines(bounds, 0.12f, 10, kBorder);

    DrawText(title, static_cast<int>(bounds.x + 18), static_cast<int>(bounds.y + 13), 15, kMuted);
    DrawTextEx(GetFontDefault(), value.c_str(), {bounds.x + 18, bounds.y + 35}, 24, 1.0f, kText);

    const float barY = bounds.y + bounds.height - 20;
    const float barX = bounds.x + 18;
    const float barWidth = bounds.width - 36;
    DrawRectangleRounded({barX, barY, barWidth, 6}, 0.5f, 8, kSurfaceRaised);
    const float normalized = static_cast<float>(std::clamp(percentage, 0.0, 100.0) / 100.0);
    if (normalized > 0.0f) {
        DrawRectangleRounded({barX, barY, barWidth * normalized, 6}, 0.5f, 8, accent);
    }
    drawTextRight(detail, bounds.x + bounds.width - 18, bounds.y + 42, 13, kMuted);
}

} // namespace

int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1040, 740, "Simple Task Manager");
    if (!IsWindowReady()) {
        return 1;
    }
    SetWindowMinSize(780, 560);
    SetExitKey(KEY_NULL);
    SetTargetFPS(30);

    SystemInfo systemInfo;
    std::shared_ptr<const SystemSnapshot> displayedSnapshot;
    std::vector<std::size_t> visibleProcesses;
    std::string query;
    bool searchFocused = false;
    int firstVisibleRow = 0;

    while (!WindowShouldClose()) {
        const auto latestSnapshot = systemInfo.getSnapshot();
        bool rebuildFilter = latestSnapshot != displayedSnapshot;
        if (rebuildFilter) {
            displayedSnapshot = latestSnapshot;
        }

        const int screenWidth = GetScreenWidth();
        const int screenHeight = GetScreenHeight();
        const float width = static_cast<float>(screenWidth);
        const float height = static_cast<float>(screenHeight);
        const Vector2 mouse = GetMousePosition();

        constexpr float padding = 26.0f;
        const Rectangle searchBounds{padding, 207.0f, width - padding * 2.0f, 42.0f};
        const float tableTop = 292.0f;
        const float rowsTop = tableTop + 43.0f;
        const float rowHeight = 29.0f;
        const float rowsBottom = height - 37.0f;
        const Rectangle rowsBounds{padding + 1.0f, rowsTop, width - padding * 2.0f - 2.0f,
                                   std::max(0.0f, rowsBottom - rowsTop)};

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            searchFocused = CheckCollisionPointRec(mouse, searchBounds);
        }
        if (searchFocused) {
            int codepoint = GetCharPressed();
            while (codepoint > 0) {
                if (codepoint >= 32 && codepoint != 127 && query.size() < 128) {
                    appendUtf8(query, codepoint);
                    rebuildFilter = true;
                }
                codepoint = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !query.empty()) {
                removeLastUtf8Character(query);
                rebuildFilter = true;
            }
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
            searchFocused = false;
        }
        if (IsKeyPressed(KEY_ESCAPE) && !query.empty()) {
            query.clear();
            rebuildFilter = true;
        }

        if (rebuildFilter && displayedSnapshot) {
            visibleProcesses.clear();
            visibleProcesses.reserve(displayedSnapshot->processes.size());
            for (std::size_t index = 0; index < displayedSnapshot->processes.size(); ++index) {
                if (matchesQuery(displayedSnapshot->processes[index], query)) {
                    visibleProcesses.push_back(index);
                }
            }
            firstVisibleRow = 0;
        }

        const int visibleRowCount = std::max(0, static_cast<int>(rowsBounds.height / rowHeight));
        const int maximumFirstRow = std::max(0,
            static_cast<int>(visibleProcesses.size()) - visibleRowCount);
        if (CheckCollisionPointRec(mouse, rowsBounds)) {
            firstVisibleRow -= static_cast<int>(std::lround(GetMouseWheelMove() * 3.0f));
        } else {
            (void)GetMouseWheelMove();
        }
        firstVisibleRow = std::clamp(firstVisibleRow, 0, maximumFirstRow);

        BeginDrawing();
        ClearBackground(kBackground);

        DrawText("System monitor", static_cast<int>(padding), 25, 28, kText);
        DrawText("Live CPU, memory, and process usage", static_cast<int>(padding), 62, 14, kMuted);

        const float cardGap = 14.0f;
        const float cardWidth = (width - padding * 2.0f - cardGap) / 2.0f;
        const SystemSnapshot emptySnapshot;
        const SystemSnapshot& snapshot = displayedSnapshot ? *displayedSnapshot : emptySnapshot;
        const std::string cpuValue = displayedSnapshot
            ? TextFormat("%.1f%%", snapshot.cpuUsage) : "Collecting...";
        const std::string cpuDetail = displayedSnapshot
            ? TextFormat("%u logical processors", snapshot.logicalProcessorCount)
            : "First sample in progress";
        const std::string memoryValue = snapshot.totalMemoryBytes > 0
            ? formatBytes(snapshot.usedMemoryBytes) + " / " + formatBytes(snapshot.totalMemoryBytes)
            : "Collecting...";
        const std::string memoryDetail = snapshot.totalMemoryBytes > 0
            ? TextFormat("%.1f%% used", snapshot.ramUsage) : "Physical memory";

        drawMetricCard({padding, 96.0f, cardWidth, 94.0f}, "CPU USAGE", cpuValue,
                       cpuDetail, snapshot.cpuUsage, kBlue);
        drawMetricCard({padding + cardWidth + cardGap, 96.0f, cardWidth, 94.0f},
                       "MEMORY", memoryValue, memoryDetail, snapshot.ramUsage, kGreen);

        DrawRectangleRounded(searchBounds, 0.16f, 10,
                             searchFocused ? kSurfaceRaised : kSurface);
        DrawRectangleRoundedLines(searchBounds, 0.16f, 10,
                                  searchFocused ? kBlue : kBorder);
        const char* searchText = query.empty() && !searchFocused
            ? "Search by process name or PID" : query.c_str();
        DrawText(searchText, static_cast<int>(searchBounds.x + 15.0f),
                 static_cast<int>(searchBounds.y + 13.0f), 14,
                 query.empty() && !searchFocused ? kMuted : kText);
        if (searchFocused && static_cast<int>(GetTime() * 2.0) % 2 == 0) {
            const float cursorX = searchBounds.x + 15.0f
                + MeasureTextEx(GetFontDefault(), query.c_str(), 14.0f, 1.0f).x + 2.0f;
            DrawLine(static_cast<int>(cursorX), static_cast<int>(searchBounds.y + 11.0f),
                     static_cast<int>(cursorX), static_cast<int>(searchBounds.y + 31.0f), kText);
        }

        const std::string processTitle = "Processes  ·  "
            + std::to_string(visibleProcesses.size()) + " shown";
        DrawText(processTitle.c_str(), static_cast<int>(padding), 264, 16, kText);
        if (displayedSnapshot) {
            const std::string total = std::to_string(snapshot.processes.size()) + " total";
            drawTextRight(total, width - padding, 266, 13, kMuted);
        }

        const Rectangle tableBounds{padding, tableTop, width - padding * 2.0f,
                                    std::max(80.0f, height - tableTop - 22.0f)};
        DrawRectangleRounded(tableBounds, 0.08f, 10, kSurface);
        DrawRectangleRoundedLines(tableBounds, 0.08f, 10, kBorder);
        DrawText("PROCESS", static_cast<int>(padding + 16.0f), 307, 11, kMuted);

        const float pidRight = width * 0.60f;
        const float cpuRight = width * 0.76f;
        const float memoryRight = width - padding - 18.0f;
        drawTextRight("PID", pidRight, 307, 11, kMuted);
        drawTextRight("CPU", cpuRight, 307, 11, kMuted);
        drawTextRight("MEMORY", memoryRight, 307, 11, kMuted);
        DrawLine(static_cast<int>(padding + 1.0f), static_cast<int>(rowsTop - 6.0f),
                 static_cast<int>(width - padding - 1.0f), static_cast<int>(rowsTop - 6.0f),
                 kBorder);

        if (!displayedSnapshot) {
            DrawText("Reading system processes...", static_cast<int>(padding + 16.0f),
                     static_cast<int>(rowsTop + 14.0f), 14, kMuted);
        } else if (visibleProcesses.empty()) {
            DrawText(query.empty() ? "No process information is available."
                                   : "No processes match this search.",
                     static_cast<int>(padding + 16.0f), static_cast<int>(rowsTop + 14.0f),
                     14, kMuted);
        } else {
            BeginScissorMode(static_cast<int>(rowsBounds.x), static_cast<int>(rowsBounds.y),
                             static_cast<int>(rowsBounds.width),
                             static_cast<int>(rowsBounds.height));
            for (int row = 0; row < visibleRowCount; ++row) {
                const int filteredIndex = firstVisibleRow + row;
                if (filteredIndex >= static_cast<int>(visibleProcesses.size())) {
                    break;
                }
                const ProcessInfo& process = snapshot.processes[
                    visibleProcesses[static_cast<std::size_t>(filteredIndex)]];
                const float rowY = rowsTop + static_cast<float>(row) * rowHeight;
                if ((filteredIndex % 2) == 1) {
                    DrawRectangle(static_cast<int>(rowsBounds.x), static_cast<int>(rowY),
                                  static_cast<int>(rowsBounds.width), static_cast<int>(rowHeight),
                                  Fade(kSurfaceRaised, 0.42f));
                }

                const std::string name = truncateText(process.name, pidRight - padding - 50.0f, 14.0f);
                DrawTextEx(GetFontDefault(), name.c_str(), {padding + 16.0f, rowY + 7.0f},
                           14.0f, 1.0f, kText);
                drawTextRight(std::to_string(process.pid), pidRight, rowY + 7.0f, 13.0f, kMuted);
                drawTextRight(TextFormat("%.1f%%", process.cpuUsage), cpuRight,
                              rowY + 7.0f, 13.0f, process.cpuUsage >= 80.0 ? kGreen : kText);
                drawTextRight(formatBytes(process.memoryBytes), memoryRight,
                              rowY + 7.0f, 13.0f, kMuted);
            }
            EndScissorMode();

            if (visibleProcesses.size() > static_cast<std::size_t>(visibleRowCount)
                && rowsBounds.height > 0.0f) {
                const float trackX = width - padding - 5.0f;
                const float trackHeight = rowsBounds.height - 8.0f;
                const float thumbHeight = std::max(22.0f,
                    trackHeight * static_cast<float>(visibleRowCount)
                        / static_cast<float>(visibleProcesses.size()));
                const float scrollRange = static_cast<float>(maximumFirstRow);
                const float thumbY = rowsBounds.y + 4.0f
                    + (scrollRange > 0.0f
                        ? (trackHeight - thumbHeight) * firstVisibleRow / scrollRange : 0.0f);
                DrawRectangleRounded({trackX, thumbY, 3.0f, thumbHeight}, 0.5f, 4,
                                     Fade(kMuted, 0.55f));
            }
        }

        DrawText("Updates once per second", static_cast<int>(padding), screenHeight - 19, 11,
                 kMuted);
        if (searchFocused) {
            drawTextRight("Enter: done  ·  Esc: clear", width - padding, screenHeight - 19,
                          11, kMuted);
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
