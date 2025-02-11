// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_EXAMPLES_ESCHER_WATERFALL_SCENES_DEMO_SCENE_H_
#define SRC_UI_EXAMPLES_ESCHER_WATERFALL_SCENES_DEMO_SCENE_H_

#include "src/ui/examples/escher/waterfall/scenes/scene.h"
#include "src/ui/lib/escher/escher.h"

class DemoScene : public Scene {
 public:
  DemoScene(Demo* demo);
  ~DemoScene();

  void Init(escher::Stage* stage) override;

  escher::Model* Update(const escher::Stopwatch& stopwatch,
                        uint64_t frame_count, escher::Stage* stage,
                        escher::PaperRenderer* renderer) override;

 private:
  std::unique_ptr<escher::Model> model_;

  escher::MaterialPtr purple_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DemoScene);
};

#endif  // SRC_UI_EXAMPLES_ESCHER_WATERFALL_SCENES_DEMO_SCENE_H_
