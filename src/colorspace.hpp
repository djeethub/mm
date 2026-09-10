#pragma once

static const float kMatrix_BT709_Limited[] = {
    1.164384f,  0.000000f,  1.596027f, -0.874202f, // Row 1 (R calculation + offset)
    1.164384f, -0.391762f, -0.812968f,  0.531668f, // Row 2 (G calculation + offset)
    1.164384f,  2.017232f,  0.000000f, -1.085631f  // Row 3 (B calculation + offset)
};        

static const float kMatrix_BT601_Limited[] = {
    1.164384f,  0.000000f,  1.596027f, -0.874202f,
    1.164384f, -0.391762f, -0.812968f,  0.531668f,
    1.164384f,  2.017232f,  0.000000f, -1.085631f
};

static const float kMatrix_BT2020_Limited[] = {
    1.164384f,  0.000000f,  1.678674f, -0.915688f,
    1.164384f, -0.187326f, -0.650424f,  0.441459f,
    1.164384f,  2.141770f,  0.000000f, -1.148145f
};

static const float kMatrix_BT709_Full[] = {
    1.000000f,  0.000000f,  1.402000f, -0.701000f,
    1.000000f, -0.344136f, -0.714136f,  0.529136f,
    1.000000f,  1.772000f,  0.000000f, -0.886000f
};

static const float kMatrix_BT601_Full[] = {
    1.000000f,  0.000000f,  1.401740f, -0.700870f,
    1.000000f, -0.343695f, -0.714169f,  0.528932f,
    1.000000f,  1.773000f,  0.000000f, -0.886500f
};

// Standard Gamut identity layout (Assuming standard sRGB destination layout)
static const float kIdentityGamut[] = {
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f,
    0.0f, 0.0f, 0.0f
};

// Gamma curve coefficients (sRGB/BT.709 standard function variables)
static const float kStandardTransferFn[] = {
    2.4f, 0.947867f, 0.0521325f, 0.0773994f, 0.04045f, 0.0f, 0.0f
};
/*
// Check if the frame dictates an HDR Transfer Function
if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
    // The frame is HDR10 (PQ Curve).
    // Note: Standard Dawn custom float formulas (a*x + b)^g don't natively compute 
    // the complex PQ curve. For pure PQ, WebGPU applications often bypass ExternalTexture matrix 
    // transforms and decode raw P010 textures inside custom WGSL fragment shaders.
} else if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
    // The frame is HLG (Hybrid Log-Gamma).
} else {
    // Frame is standard SDR (Rec. 709 / Rec. 601)
    externalDesc.srcTransferFunctionParameters = kStandardTransferFn;
}
*/