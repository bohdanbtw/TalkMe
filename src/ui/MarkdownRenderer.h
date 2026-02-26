#pragma once
#include <imgui.h>
#include <string>
#include <vector>

namespace TalkMe::UI {

struct TextSegment {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool mention = false;
    bool url = false;
};

std::vector<TextSegment> ParseMarkdown(const std::string& input);

void RenderMarkdownText(const std::string& text, float wrapWidth = 0.0f);

} // namespace TalkMe::UI
