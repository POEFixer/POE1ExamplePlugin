#pragma once
// ============================================================================
// Shell — the panel around the lessons: a status strip on top, the lesson
// list on the left, the open lesson on the right (title, What / When /
// Limits, status line, demo, source chip and Copy snippet).
// ============================================================================
#include "App.h"

namespace ex {

void DrawPanel(App& app, const P1Frame& f);

}  // namespace ex
