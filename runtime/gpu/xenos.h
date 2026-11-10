#pragma once
// Xenos (Xbox 360 GPU) register indices and field helpers used by the renderer.
// Register index = (MMIO offset) / 4; the command processor keeps them in its register file.
// Names and layouts follow Xenia's register_table.inc / xenos.h.
#include <cstdint>

namespace XeReg
{
    // Render backend: surfaces (EDRAM)
    constexpr uint32_t RB_SURFACE_INFO = 0x2000;      // pitch 0:13, msaa 16:17
    constexpr uint32_t RB_COLOR_INFO = 0x2001;        // edram base 0:11 (tiles), format 16:19, exp bias 20:25
    constexpr uint32_t RB_DEPTH_INFO = 0x2002;        // edram base 0:11, format bit 16
    constexpr uint32_t RB_COLOR1_INFO = 0x2003;
    constexpr uint32_t RB_COLOR2_INFO = 0x2004;
    constexpr uint32_t RB_COLOR3_INFO = 0x2005;
    constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080;
    constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL = 0x2081;
    constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR = 0x2082;
    constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100;
    constexpr uint32_t VGT_MIN_VTX_INDX = 0x2101;
    constexpr uint32_t VGT_INDX_OFFSET = 0x2102;
    constexpr uint32_t RB_COLOR_MASK = 0x2104;
    constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;   // then XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET
    constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180;
    constexpr uint32_t SQ_CONTEXT_MISC = 0x2181;
    constexpr uint32_t SQ_INTERPOLATOR_CNTL = 0x2182;
    constexpr uint32_t VGT_DRAW_INITIATOR = 0x21FC;
    constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
    constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;
    constexpr uint32_t RB_COLORCONTROL = 0x2202;
    constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204;
    constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
    constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;
    constexpr uint32_t RB_MODECONTROL = 0x2208;       // edram mode 0:2
    constexpr uint32_t PA_SC_AA_CONFIG = 0x2301;
    // Resolve (EDRAM -> memory copy)
    constexpr uint32_t RB_COPY_CONTROL = 0x2318;      // src 0:2, color clear bit 8, depth clear bit 9, command 20:21
    constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
    constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;   // pitch 0:13, height 16:29
    constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;    // endian 0:2, format 7:12
    constexpr uint32_t RB_DEPTH_CLEAR = 0x231D;
    constexpr uint32_t RB_COLOR_CLEAR = 0x231E;
    constexpr uint32_t RB_COLOR_CLEAR_LO = 0x231F;
    // Constants
    constexpr uint32_t ALU_CONSTANTS = 0x4000;        // 512 float4: VS 0-255, PS 256-511
    constexpr uint32_t FETCH_CONSTANTS = 0x4800;      // 32 texture (6 dwords) / 96 vertex (2 dwords)
    constexpr uint32_t BOOL_CONSTANTS = 0x4900;
    constexpr uint32_t LOOP_CONSTANTS = 0x4908;
}

namespace Xenos
{
    enum class PrimitiveType : uint32_t
    {
        None = 0, PointList = 1, LineList = 2, LineStrip = 3, TriangleList = 4, TriangleFan = 5,
        TriangleStrip = 6, RectangleList = 8, LineLoop = 12, QuadList = 13, QuadStrip = 14,
    };

    enum class SourceSelect : uint32_t { Dma = 0, Immediate = 1, AutoIndex = 2 };

    // RB_MODECONTROL.edram_mode
    enum class EdramMode : uint32_t { Ignore = 0, ColorDepth = 4, Depth = 5, Copy = 6 };

    struct DrawInitiator
    {
        uint32_t value;
        PrimitiveType Primitive() const { return PrimitiveType(value & 0x3F); }
        SourceSelect Source() const { return SourceSelect((value >> 6) & 3); }
        bool Index32() const { return (value >> 11) & 1; }
        uint32_t IndexCount() const { return value >> 16; }
    };

    inline const char* PrimitiveName(PrimitiveType p)
    {
        switch (p)
        {
        case PrimitiveType::PointList: return "points";
        case PrimitiveType::LineList: return "lines";
        case PrimitiveType::LineStrip: return "line-strip";
        case PrimitiveType::TriangleList: return "triangles";
        case PrimitiveType::TriangleFan: return "tri-fan";
        case PrimitiveType::TriangleStrip: return "tri-strip";
        case PrimitiveType::RectangleList: return "rects";
        case PrimitiveType::LineLoop: return "line-loop";
        case PrimitiveType::QuadList: return "quads";
        case PrimitiveType::QuadStrip: return "quad-strip";
        default: return "?";
        }
    }
}
