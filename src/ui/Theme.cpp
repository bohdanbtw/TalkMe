#include "Theme.h"
#include "Styles.h"
#include "../core/ConfigManager.h"

namespace TalkMe::UI {

    void ApplyAppStyle() {
        int savedTheme = ConfigManager::Get().LoadTheme(0);
        if (savedTheme < 0 || savedTheme >= (int)ThemeId::Count)
            savedTheme = 0;
        Styles::SetTheme((ThemeId)savedTheme);
    }
}
