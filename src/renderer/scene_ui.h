#pragma once

#include "protocol.h"

// Layout of one renderer window, in that window's pixels. Buttons keep a
// fixed size. The video or photo occupies whatever space is left.

struct UiRect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;

    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

enum ButtonId : int {
    kButtonNone = 0,
    kButtonPlay = 1,
    kButtonFit = 2,
    kButtonActual = 3,
    kButtonStart = 4,
    kButtonStop = 5,
    kButtonReset = 6,
    kButtonNote = 7,
};

struct SceneButtons {
    int id[8] = {};
    UiRect rect[8] = {};
    int count = 0;
};

struct ViewLayout {
    float width = 0;
    float height = 0;
    float scale = 1;
    UiRect well{};
    float barTop = 0;
};

struct SceneState {
    int hot = kButtonNone;
    int pressed = kButtonNone;
    bool playing = true;
    double mediaSeconds = 0;
    double mediaDuration = 0;
    bool actualSize = false;
    int runState = 0;
    int clicks = 0;
    int noteCount = 0;
    char notes[5][48] = {};
};

void layoutScene(int scene, float width, float height, float scale, ViewLayout* view, SceneButtons* buttons);
UiRect mediaWell();
void layoutButtons(int scene, SceneButtons* out);
const char* buttonLabel(int id, const SceneState& state);
bool buttonPrimary(int id, const SceneState& state);
const char* sceneTitle(int scene);
void applyPointer(SceneState* state, int scene, float x, float y, uint32_t action);
void applyPointer(SceneState* state, int scene, float x, float y, uint32_t action, float viewW, float viewH, float scale);
