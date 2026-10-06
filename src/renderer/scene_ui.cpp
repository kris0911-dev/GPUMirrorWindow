#include "scene_ui.h"

#include <algorithm>
#include <cstdio>

namespace {

void addButton(SceneButtons* buttons, int id, float x, float y, float w, float h) {
    if (buttons->count >= 8) {
        return;
    }
    const int index = buttons->count++;
    buttons->id[index] = id;
    buttons->rect[index] = {x, y, w, h};
}

int buttonAt(const SceneButtons& buttons, float x, float y) {
    for (int i = 0; i < buttons.count; ++i) {
        if (buttons.rect[i].contains(x, y)) {
            return buttons.id[i];
        }
    }
    return kButtonNone;
}

void activate(SceneState* state, int id) {
    switch (id) {
    case kButtonPlay:
        state->playing = !state->playing;
        break;
    case kButtonFit:
        state->actualSize = false;
        break;
    case kButtonActual:
        state->actualSize = true;
        break;
    case kButtonStart:
        state->runState = 1;
        state->clicks += 1;
        break;
    case kButtonStop:
        state->runState = 2;
        state->clicks += 1;
        break;
    case kButtonReset:
        state->runState = 0;
        state->clicks = 0;
        state->noteCount = 0;
        break;
    case kButtonNote:
        if (state->noteCount < 5) {
            std::snprintf(state->notes[state->noteCount], sizeof(state->notes[0]), "Note %d", state->noteCount + 1);
            state->noteCount += 1;
        }
        state->clicks += 1;
        break;
    default:
        break;
    }
}

}  // namespace

void layoutScene(int scene, float width, float height, float scale, ViewLayout* view, SceneButtons* buttons) {
    if (!view || !buttons) {
        return;
    }
    if (width < 1.f) {
        width = 1.f;
    }
    if (height < 1.f) {
        height = 1.f;
    }
    if (scale < 0.5f) {
        scale = 0.5f;
    }
    view->width = width;
    view->height = height;
    view->scale = scale;
    buttons->count = 0;

    const float margin = 20.f * scale;
    const float header = 72.f * scale;
    if (scene == 0 || scene == 1) {
        const float bar = 64.f * scale;
        const float wellH = std::max(1.f, height - header - bar);
        view->well = {margin, header, std::max(1.f, width - margin * 2.f), wellH};
        view->barTop = header + wellH;
        const float y = view->barTop + 14.f * scale;
        const float h = 36.f * scale;
        if (scene == 0) {
            addButton(buttons, kButtonPlay, margin, y, 112.f * scale, h);
        } else {
            addButton(buttons, kButtonFit, margin, y, 96.f * scale, h);
            addButton(buttons, kButtonActual, margin + 108.f * scale, y, 140.f * scale, h);
        }
        return;
    }

    view->barTop = header;
    view->well = {margin, header, std::max(1.f, width - margin * 2.f), std::max(1.f, height - header - margin)};
    const float bw = 132.f * scale;
    const float bh = 40.f * scale;
    const float gap = 12.f * scale;
    float x = margin;
    float y = header + 8.f * scale;
    const int ids[] = {kButtonStart, kButtonStop, kButtonReset, kButtonNote};
    for (int id : ids) {
        if (x + bw > width - margin && x > margin + 1.f) {
            x = margin;
            y += bh + gap;
        }
        addButton(buttons, id, x, y, bw, bh);
        x += bw + gap;
    }
}

UiRect mediaWell() {
    ViewLayout view{};
    SceneButtons buttons{};
    layoutScene(0, (float)kSurfaceWidth, (float)kSurfaceHeight, 1.f, &view, &buttons);
    return view.well;
}

void layoutButtons(int scene, SceneButtons* out) {
    ViewLayout view{};
    layoutScene(scene, (float)kSurfaceWidth, (float)kSurfaceHeight, 1.f, &view, out);
}

const char* buttonLabel(int id, const SceneState& state) {
    switch (id) {
    case kButtonPlay:
        return state.playing ? "Pause" : "Play";
    case kButtonFit:
        return "Fit";
    case kButtonActual:
        return "Actual size";
    case kButtonStart:
        return "Start";
    case kButtonStop:
        return "Stop";
    case kButtonReset:
        return "Reset";
    case kButtonNote:
        return "Add note";
    default:
        return "";
    }
}

bool buttonPrimary(int id, const SceneState& state) {
    if (id == kButtonPlay) {
        return true;
    }
    if (id == kButtonFit) {
        return !state.actualSize;
    }
    if (id == kButtonActual) {
        return state.actualSize;
    }
    if (id == kButtonStart) {
        return state.runState == 1;
    }
    if (id == kButtonStop) {
        return state.runState == 2;
    }
    return false;
}

const char* sceneTitle(int scene) {
    static const char* kTitles[] = {"Video", "Image", "Controls"};
    if (scene < 0 || scene > 2) {
        return "Renderer";
    }
    return kTitles[scene];
}

void applyPointer(SceneState* state, int scene, float x, float y, uint32_t action, float viewW, float viewH, float scale) {
    if (!state) {
        return;
    }
    ViewLayout view{};
    SceneButtons buttons{};
    layoutScene(scene, viewW, viewH, scale, &view, &buttons);
    const int id = buttonAt(buttons, x, y);
    if (action == kPointerMove) {
        state->hot = id;
        return;
    }
    if (action == kPointerDown) {
        state->pressed = id;
        state->hot = id;
        return;
    }
    if (action == kPointerUp) {
        const int pressed = state->pressed;
        state->pressed = kButtonNone;
        state->hot = id;
        if (pressed != kButtonNone && pressed == id) {
            activate(state, pressed);
        }
    }
}

void applyPointer(SceneState* state, int scene, float x, float y, uint32_t action) {
    applyPointer(state, scene, x, y, action, (float)kSurfaceWidth, (float)kSurfaceHeight, 1.f);
}
