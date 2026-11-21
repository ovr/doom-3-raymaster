// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// texture_color for particles (RB_BindSoftParticle): it fades where it meets the scene's depth. Particles have no
// joints, so SKINNED changes nothing and there is no skinned variant.

#undef SKINNED
#define SOFT_PARTICLE 1
#include "texture_color.hlsl"
