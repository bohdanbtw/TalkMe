#include "MarkdownRenderer.h"
#include "Styles.h"
#include <cstring>
#include <algorithm>

namespace TalkMe::UI {

std::vector<TextSegment> ParseMarkdown(const std::string& input) {
    std::vector<TextSegment> segments;
    size_t i = 0;
    std::string current;
    bool inBold = false, inItalic = false, inCode = false;

    auto flush = [&]() {
        if (!current.empty()) {
            TextSegment seg;
            seg.text = current;
            seg.bold = inBold;
            seg.italic = inItalic;
            seg.code = inCode;
            if (current.size() > 1 && current[0] == '@') seg.mention = true;
            if (current.find("http") == 0) seg.url = true;
            segments.push_back(seg);
            current.clear();
        }
    };

    while (i < input.size()) {
        // Code block (```)
        if (i + 2 < input.size() && input[i] == '`' && input[i+1] == '`' && input[i+2] == '`') {
            flush();
            inCode = !inCode;
            i += 3;
            continue;
        }
        // Inline code (`)
        if (input[i] == '`' && !inCode) {
            flush();
            size_t end = input.find('`', i + 1);
            if (end != std::string::npos) {
                TextSegment seg;
                seg.text = input.substr(i + 1, end - i - 1);
                seg.code = true;
                segments.push_back(seg);
                i = end + 1;
                continue;
            }
        }
        // Bold (**)
        if (i + 1 < input.size() && input[i] == '*' && input[i+1] == '*' && !inCode) {
            flush();
            inBold = !inBold;
            i += 2;
            continue;
        }
        // Italic (*)
        if (input[i] == '*' && !inCode && (i + 1 >= input.size() || input[i+1] != '*')) {
            flush();
            inItalic = !inItalic;
            i += 1;
            continue;
        }
        current += input[i];
        i++;
    }
    flush();
    return segments;
}

void RenderMarkdownText(const std::string& text, float wrapWidth) {
    auto segments = ParseMarkdown(text);
    bool first = true;

    for (const auto& seg : segments) {
        if (!first && !seg.text.empty() && seg.text[0] != '\n')
            ImGui::SameLine(0, 0);
        first = false;

        if (seg.code) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.3f, 1.0f));
            // Code background
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 sz = ImGui::CalcTextSize(seg.text.c_str());
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(pos.x - 2, pos.y - 1), ImVec2(pos.x + sz.x + 2, pos.y + sz.y + 1),
                IM_COL32(40, 40, 48, 200), 3.0f);
            ImGui::Text("%s", seg.text.c_str());
            ImGui::PopStyleColor();
        } else if (seg.mention) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
            ImGui::Text("%s", seg.text.c_str());
            ImGui::PopStyleColor();
        } else if (seg.bold && seg.italic) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::Text("%s", seg.text.c_str());
            ImGui::PopStyleColor();
        } else if (seg.bold) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::Text("%s", seg.text.c_str());
            ImGui::PopStyleColor();
        } else if (seg.italic) {
            ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
            ImGui::Text("%s", seg.text.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextPrimary());
            if (wrapWidth > 0)
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
            ImGui::TextWrapped("%s", seg.text.c_str());
            if (wrapWidth > 0)
                ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }
}

} // namespace TalkMe::UI
