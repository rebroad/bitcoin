/**********************************************************************
 * Copyright (c) 2018 Pieter Wuille, Greg Maxwell, Gleb Naumenko      *
 * Distributed under the MIT software license, see the accompanying   *
 * file LICENSE or http://www.opensource.org/licenses/mit-license.php.*
 **********************************************************************/

/* This file was substantially auto-generated by doc/gen_params.sage. */

#if !defined(DISABLE_FIELD_41) || !defined(DISABLE_FIELD_42) || !defined(DISABLE_FIELD_43) || !defined(DISABLE_FIELD_44) || !defined(DISABLE_FIELD_45) || !defined(DISABLE_FIELD_46) || !defined(DISABLE_FIELD_47) || !defined(DISABLE_FIELD_48)

#include "clmul_common_impl.h"

#include "../int_utils.h"
#include "../lintrans.h"
#include "../sketch_impl.h"

#endif

#include "../sketch.h"

namespace {
#ifndef DISABLE_FIELD_41
// 41 bit field
typedef RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 6, 5> StatTable41;
constexpr StatTable41 SQR_TABLE_41({0x1, 0x4, 0x10, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x100000, 0x400000, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x100000000, 0x400000000, 0x1000000000, 0x4000000000, 0x10000000000, 0x12, 0x48, 0x120, 0x480, 0x1200, 0x4800, 0x12000, 0x48000, 0x120000, 0x480000, 0x1200000, 0x4800000, 0x12000000, 0x48000000, 0x120000000, 0x480000000, 0x1200000000, 0x4800000000, 0x12000000000, 0x8000000012});
constexpr StatTable41 SQR2_TABLE_41({0x1, 0x10, 0x100, 0x1000, 0x10000, 0x100000, 0x1000000, 0x10000000, 0x100000000, 0x1000000000, 0x10000000000, 0x48, 0x480, 0x4800, 0x48000, 0x480000, 0x4800000, 0x48000000, 0x480000000, 0x4800000000, 0x8000000012, 0x104, 0x1040, 0x10400, 0x104000, 0x1040000, 0x10400000, 0x104000000, 0x1040000000, 0x10400000000, 0x4000000048, 0x492, 0x4920, 0x49200, 0x492000, 0x4920000, 0x49200000, 0x492000000, 0x4920000000, 0x9200000012, 0x12000000104});
constexpr StatTable41 SQR4_TABLE_41({0x1, 0x10000, 0x100000000, 0x480, 0x4800000, 0x8000000012, 0x104000, 0x1040000000, 0x4920, 0x49200000, 0x12000000104, 0x1001000, 0x10010000000, 0x48048, 0x480480000, 0x4800001040, 0x10410400, 0x4104000048, 0x492492, 0x4924920000, 0x9200010002, 0x100000100, 0x1000480, 0x10004800000, 0x8000048012, 0x480104000, 0x1040001040, 0x10404920, 0x4049200048, 0x12000492104, 0x4921001000, 0x10010010010, 0x100148048, 0x1480480480, 0x4804805840, 0x8058410412, 0x410410414c, 0x10414d2492, 0x14d24924920, 0x9249259202, 0x12592000004});
constexpr StatTable41 SQR8_TABLE_41({0x1, 0x10410400, 0x100148048, 0x13040000104, 0x11044801040, 0x924da49202, 0x9680490002, 0x82510514c, 0x8481485932, 0xc83144d832, 0x134d34d6db6, 0x18010048012, 0x165db20004c, 0x4800597052, 0x10131135cd0, 0xcd6cc30d32, 0x160586101cc, 0x15c64969da8, 0x179715681cc, 0x12f3c0ffc74, 0xc7dd3dd3ce, 0x10014c968, 0x1b040048116, 0x35d6801044, 0xda4deda6d0, 0x1de94c85852, 0x1083500114c, 0xc4c9685dfa, 0x18515c6d592, 0x17de69aed7e, 0x16c6c8a6c6c, 0x165cfe1044c, 0xdb004cf018, 0x7075031c98, 0x1d9a90b0d72, 0x1bb2485caee, 0x1cbe4dfd48a, 0x1f1540b7400, 0xc62bc7fd02, 0x147b5103f2e, 0x390ee8bcc6});
constexpr StatTable41 SQR16_TABLE_41({0x1, 0x61deee38fe, 0xe00adae2e, 0x1ea53eaa95a, 0x503e540566, 0xabc8e7f89a, 0x1bf760d86ac, 0x94cce9c722, 0x15c8006ee5c, 0x7aba20c1da, 0x12662a9603e, 0x5fe76acec4, 0x1e6beca9e42, 0x1efc8f7a000, 0x165997c6d7e, 0xee947a07ee, 0xd9bd741142, 0xaa304566c0, 0x5fe336e356, 0x11f1021b80c, 0xd34e5a1674, 0x99ed56b9dc, 0x9afae0eca, 0x1a5830b390e, 0x1be1a63eb7e, 0x141e77e141c, 0xee3be92168, 0xa93823d65c, 0x18a59f4b19c, 0xce69942af6, 0x3f7b319c0e, 0xba83a4a7b4, 0x7da4b6fcde, 0x17f79268f10, 0x1222602d048, 0x1b4b2f326b8, 0x159abff0786, 0xb35534a7a2, 0x84bbc48050, 0x173d5cbf330, 0x2897dd6f58});
constexpr StatTable41 QRT_TABLE_41({0, 0x1599a5e0b0, 0x1599a5e0b2, 0x105c119e0, 0x1599a5e0b6, 0x1a2030452a6, 0x105c119e8, 0x1a307c55b2e, 0x1599a5e0a6, 0x1ee3f47bc8e, 0x1a203045286, 0x400808, 0x105c119a8, 0x1a3038573a6, 0x1a307c55bae, 0x4d2882a520, 0x1599a5e1a6, 0x1ffbaa0b720, 0x1ee3f47be8e, 0x4d68c22528, 0x1a203045686, 0x200006, 0x400008, 0x1b79a21b200, 0x105c109a8, 0x1ef3886a526, 0x1a3038553a6, 0x1b692209200, 0x1a307c51bae, 0x5d99a4e1a6, 0x4d28822520, 0x185e109ae, 0x1599a4e1a6, 0x4e3f43be88, 0x1ffbaa2b720, 0x4000000000, 0x1ee3f43be8e, 0x18000000006, 0x4d68ca2528, 0xa203145680, 0x1a203145686});
typedef Field<uint64_t, 41, 9, StatTable41, &SQR_TABLE_41, &SQR2_TABLE_41, &SQR4_TABLE_41, &SQR8_TABLE_41, &SQR16_TABLE_41, &QRT_TABLE_41, IdTrans, &ID_TRANS, &ID_TRANS> Field41;
typedef FieldTri<uint64_t, 41, 3, RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 6, 5>, &SQR_TABLE_41, &SQR2_TABLE_41, &SQR4_TABLE_41, &SQR8_TABLE_41, &SQR16_TABLE_41, &QRT_TABLE_41, IdTrans, &ID_TRANS, &ID_TRANS> FieldTri41;
#endif

#ifndef DISABLE_FIELD_42
// 42 bit field
typedef RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 6, 6> StatTable42;
constexpr StatTable42 SQR_TABLE_42({0x1, 0x4, 0x10, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x100000, 0x400000, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x100000000, 0x400000000, 0x1000000000, 0x4000000000, 0x10000000000, 0x81, 0x204, 0x810, 0x2040, 0x8100, 0x20400, 0x81000, 0x204000, 0x810000, 0x2040000, 0x8100000, 0x20400000, 0x81000000, 0x204000000, 0x810000000, 0x2040000000, 0x8100000000, 0x20400000000, 0x1000000102, 0x4000000408, 0x10000001020});
constexpr StatTable42 SQR2_TABLE_42({0x1, 0x10, 0x100, 0x1000, 0x10000, 0x100000, 0x1000000, 0x10000000, 0x100000000, 0x1000000000, 0x10000000000, 0x204, 0x2040, 0x20400, 0x204000, 0x2040000, 0x20400000, 0x204000000, 0x2040000000, 0x20400000000, 0x4000000408, 0x4001, 0x40010, 0x400100, 0x4001000, 0x40010000, 0x400100000, 0x4001000000, 0x10000081, 0x100000810, 0x1000008100, 0x10000081000, 0x810204, 0x8102040, 0x81020400, 0x810204000, 0x8102040000, 0x1020400102, 0x10204001020, 0x2040010004, 0x20400100040, 0x4001000008});
constexpr StatTable42 SQR4_TABLE_42({0x1, 0x10000, 0x100000000, 0x2040, 0x20400000, 0x4000000408, 0x4001000, 0x10000081, 0x810204, 0x8102040000, 0x20400100040, 0x1000000100, 0x1020400, 0x10204000000, 0x200001, 0x2000010000, 0x100040800, 0x408002040, 0x20408002, 0x4080020408, 0x204000020, 0x204001, 0x2040010000, 0x100040010, 0x400102040, 0x1020408100, 0x4081020008, 0x10200000020, 0x80, 0x800000, 0x8000000000, 0x102000, 0x1020000000, 0x20008, 0x200080000, 0x800004080, 0x40810200, 0x8102000810, 0x20008000040, 0x8102, 0x81020000, 0x10200001020});
constexpr StatTable42 SQR8_TABLE_42({0x1, 0x100040800, 0x1020000000, 0x10200001000, 0x2040810000, 0x408102040, 0x4080000408, 0x10000000, 0x8002040810, 0x20008000, 0x10200080000, 0x40000004, 0x20408000040, 0x4000020000, 0x204000, 0x8002040010, 0x408102, 0x200081020, 0x40810000, 0x2000, 0x81000408, 0x4001, 0x2040010, 0x1020008002, 0x10204081020, 0x800004, 0x20000000000, 0x4081000400, 0x10000081, 0x100040010, 0x8102, 0x10000000020, 0x40010200, 0x408000000, 0x4080000400, 0x810204000, 0x102040810, 0x1020000102, 0x4000000, 0x2000810204, 0x8002000, 0x4080020000});
constexpr StatTable42 SQR16_TABLE_42({0x1, 0x40800204, 0x2000800, 0x20008002040, 0x408100, 0x20000, 0x10004081020, 0x10000081, 0x40010204, 0x8102000000, 0x400000000, 0x1020008102, 0x1020408, 0x204081020, 0x200001, 0x10200, 0x102000010, 0x20408100000, 0x1020408002, 0x4000020000, 0x204000000, 0x204001, 0x2000800004, 0x8100000010, 0x400102000, 0x8002, 0x4080020000, 0x10000001000, 0x80, 0x2040010200, 0x100040000, 0x400100040, 0x20408000, 0x1000000, 0x204080020, 0x800004080, 0x2000810200, 0x8100000810, 0x20000000000, 0x1000408002, 0x81020400, 0x10204081000});
constexpr StatTable42 QRT_TABLE_42({0x810200080, 0x120810806, 0x120810804, 0x1068c1a1000, 0x120810800, 0x34005023008, 0x1068c1a1008, 0x800004080, 0x120810810, 0x162818a10, 0x34005023028, 0x42408a14, 0x1068c1a1048, 0x1001040, 0x800004000, 0xb120808906, 0x120810910, 0x34000020068, 0x162818810, 0x68c021400, 0x34005023428, 0x10004000, 0x42408214, 0x162418214, 0x1068c1a0048, 0xb002018116, 0x1003040, 0x10008180448, 0x800000000, 0x62c08b04, 0xb120800906, 0x2408d1a3060, 0x120800910, 0x34401003028, 0x34000000068, 0, 0x162858810, 0xa042058116, 0x68c0a1400, 0x8162858806, 0x34005123428, 0x3068c0a1468});
typedef Field<uint64_t, 42, 129, StatTable42, &SQR_TABLE_42, &SQR2_TABLE_42, &SQR4_TABLE_42, &SQR8_TABLE_42, &SQR16_TABLE_42, &QRT_TABLE_42, IdTrans, &ID_TRANS, &ID_TRANS> Field42;
typedef FieldTri<uint64_t, 42, 7, RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 6, 6>, &SQR_TABLE_42, &SQR2_TABLE_42, &SQR4_TABLE_42, &SQR8_TABLE_42, &SQR16_TABLE_42, &QRT_TABLE_42, IdTrans, &ID_TRANS, &ID_TRANS> FieldTri42;
#endif

#ifndef DISABLE_FIELD_43
// 43 bit field
typedef RecLinTrans<uint64_t, 6, 6, 6, 5, 5, 5, 5, 5> StatTable43;
constexpr StatTable43 SQR_TABLE_43({0x1, 0x4, 0x10, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x100000, 0x400000, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x100000000, 0x400000000, 0x1000000000, 0x4000000000, 0x10000000000, 0x40000000000, 0xb2, 0x2c8, 0xb20, 0x2c80, 0xb200, 0x2c800, 0xb2000, 0x2c8000, 0xb20000, 0x2c80000, 0xb200000, 0x2c800000, 0xb2000000, 0x2c8000000, 0xb20000000, 0x2c80000000, 0xb200000000, 0x2c800000000, 0x32000000059, 0x4800000013d, 0x20000000446});
constexpr StatTable43 SQR2_TABLE_43({0x1, 0x10, 0x100, 0x1000, 0x10000, 0x100000, 0x1000000, 0x10000000, 0x100000000, 0x1000000000, 0x10000000000, 0xb2, 0xb20, 0xb200, 0xb2000, 0xb20000, 0xb200000, 0xb2000000, 0xb20000000, 0xb200000000, 0x32000000059, 0x20000000446, 0x4504, 0x45040, 0x450400, 0x4504000, 0x45040000, 0x450400000, 0x4504000000, 0x45040000000, 0x504000002c8, 0x4000002efa, 0x4000002efa0, 0x2ef8c8, 0x2ef8c80, 0x2ef8c800, 0x2ef8c8000, 0x2ef8c80000, 0x2ef8c800000, 0x6f8c800013d, 0x78c80001025, 0xc800010117, 0x48000101129});
constexpr StatTable43 SQR4_TABLE_43({0x1, 0x10000, 0x100000000, 0xb20, 0xb200000, 0x32000000059, 0x450400, 0x4504000000, 0x4000002efa0, 0x2ef8c8000, 0x78c80001025, 0x10110010, 0x11001000b2, 0x1000b2b920, 0xb2b920b200, 0x120b204545f, 0x20454554046, 0x45540506efa, 0x506ed4df68, 0x6d4df6a79f5, 0x76a79c80133, 0x1c801010007, 0x101000b2100, 0xb210b2b20, 0x10b2b204504, 0x320450f655d, 0x50f654106c8, 0x54106efcb4c, 0x6efcb696320, 0x369631c93e1, 0x31c93ff9d8c, 0x3ff9d91a2a2, 0x591a2b9839b, 0x2b983b98dd4, 0x3b98dc651b0, 0x5c651a971e9, 0x1a971c9c0ba, 0x1c9c0b5853e, 0xb585322d78, 0x5322d7c6430, 0x57c6416617d, 0x4166159c82c, 0x159c8000b6c});
constexpr StatTable43 SQR8_TABLE_43({0x1, 0x20454554046, 0x591a2b9839b, 0x722ff9f6fe9, 0x6a269c1eb12, 0xa3ce9f234e, 0x4d9ba8aae0b, 0x392cf0cf99b, 0x465f8594525, 0x4f9c1fb1524, 0x3b1a1dd441c, 0x381edd42255, 0x37a599424b, 0x554caee8670, 0x5335bb91d81, 0x69288c8a1a3, 0x3df2b6e68e5, 0x75330d31d56, 0x51a604b090c, 0x32e0d5a7ca3, 0x41eb9d4896e, 0x633e2855c9f, 0x4780d70e32f, 0x73b0cd728c3, 0x16627402bad, 0x4418f2a818a, 0x5cdd06cf7e5, 0x2a8da97a3ae, 0x446864a8976, 0x5a7bcbd45ea, 0x4034a4b8b04, 0x6bdaac9c5fa, 0x18769ce3a67, 0x560278257c, 0x41c06d6b64c, 0x69f6f61bd4b, 0x16cc45f84fd, 0x53f6b42f0f0, 0x6cac3d234f3, 0x1f94e8f24d5, 0x319342c7148, 0x8685bca86d, 0x6b694a6ea66});
constexpr StatTable43 SQR16_TABLE_43({0x1, 0x1ce77599049, 0x191715250a, 0xc1573d8dff, 0x118e73ab5e4, 0x4b6a83225fe, 0x72b4bc8e0f5, 0x4a4b2b6bb02, 0x66daf4741e9, 0x50baba19898, 0x5eb38771912, 0x6fb458aad3c, 0x5ce3b10bde9, 0x5575f3498f0, 0x5f075aa8a0a, 0x41d0aa8ee20, 0x609e3c78c28, 0xe2e45a8018, 0x523ac062837, 0x738388a569d, 0x6616ec46da9, 0x1a75cc16d96, 0x49b0b43bbc3, 0x400416b3c9a, 0x25813f41ffe, 0x309fdb9d0bc, 0x489f45b2cbf, 0xa141f4f88e, 0x739e0d11fb3, 0x44971f51cc0, 0x6490576e60e, 0x6c6674c5355, 0x6978126a4e1, 0x3d04eae5a5, 0x312eed633f2, 0x1de4b98d6b9, 0x118a106fb0a, 0x26dae025f4, 0x5c179312ebb, 0x75870ef1921, 0x60e9fed95c0, 0x209ab92427a, 0x1c5014a1937});
constexpr StatTable43 QRT_TABLE_43({0x2bccc2d6f6c, 0x4bccc2d6f54, 0x4bccc2d6f56, 0x7cc7bc61df0, 0x4bccc2d6f52, 0x7d13b404b10, 0x7cc7bc61df8, 0x37456e9ac5a, 0x4bccc2d6f42, 0x4e042c6a6, 0x7d13b404b30, 0x4a56de9ef4c, 0x7cc7bc61db8, 0x14bc18d8e, 0x37456e9acda, 0x7c89f84fb1e, 0x4bccc2d6e42, 0x7ffae40d210, 0x4e042c4a6, 0x366f45dd06, 0x7d13b404f30, 0x496fcaf8cca, 0x4a56de9e74c, 0x370b62b6af4, 0x7cc7bc60db8, 0x1498185a8, 0x14bc1ad8e, 0x7e602c46a98, 0x37456e9ecda, 0x36ccc2c6e74, 0x7c89f847b1e, 0x7e27d06d516, 0x4bccc2c6e42, 0x7f93302c396, 0x7ffae42d210, 0x3dd3440706, 0x4e046c4a6, 0x78bbc09da36, 0x366f4ddd06, 0, 0x7d13b504f30, 0x8bbc09da00, 0x496fc8f8cca});
typedef Field<uint64_t, 43, 89, StatTable43, &SQR_TABLE_43, &SQR2_TABLE_43, &SQR4_TABLE_43, &SQR8_TABLE_43, &SQR16_TABLE_43, &QRT_TABLE_43, IdTrans, &ID_TRANS, &ID_TRANS> Field43;
#endif

#ifndef DISABLE_FIELD_44
// 44 bit field
typedef RecLinTrans<uint64_t, 6, 6, 6, 6, 5, 5, 5, 5> StatTable44;
constexpr StatTable44 SQR_TABLE_44({0x1, 0x4, 0x10, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x100000, 0x400000, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x100000000, 0x400000000, 0x1000000000, 0x4000000000, 0x10000000000, 0x40000000000, 0x21, 0x84, 0x210, 0x840, 0x2100, 0x8400, 0x21000, 0x84000, 0x210000, 0x840000, 0x2100000, 0x8400000, 0x21000000, 0x84000000, 0x210000000, 0x840000000, 0x2100000000, 0x8400000000, 0x21000000000, 0x84000000000, 0x10000000042, 0x40000000108});
constexpr StatTable44 SQR2_TABLE_44({0x1, 0x10, 0x100, 0x1000, 0x10000, 0x100000, 0x1000000, 0x10000000, 0x100000000, 0x1000000000, 0x10000000000, 0x21, 0x210, 0x2100, 0x21000, 0x210000, 0x2100000, 0x21000000, 0x210000000, 0x2100000000, 0x21000000000, 0x10000000042, 0x401, 0x4010, 0x40100, 0x401000, 0x4010000, 0x40100000, 0x401000000, 0x4010000000, 0x40100000000, 0x1000000084, 0x10000000840, 0x8421, 0x84210, 0x842100, 0x8421000, 0x84210000, 0x842100000, 0x8421000000, 0x84210000000, 0x42100000108, 0x21000001004, 0x10000010002});
constexpr StatTable44 SQR4_TABLE_44({0x1, 0x10000, 0x100000000, 0x210, 0x2100000, 0x21000000000, 0x40100, 0x401000000, 0x10000000840, 0x8421000, 0x84210000000, 0x100001, 0x1000010000, 0x100002100, 0x21000210, 0x10002100042, 0x21000401000, 0x4010040100, 0x401008421, 0x10084210840, 0x42108421108, 0x84211000010, 0x10000000001, 0x31000, 0x310000000, 0x611, 0x6110000, 0x61100000000, 0xc4310, 0xc43100000, 0x31000001844, 0x18421100, 0x84211000021, 0x10000310001, 0x3100031000, 0x310006110, 0x61100611, 0x110061100c6, 0x61100c43100, 0xc4310c4310, 0x10c43118423, 0x31184210844, 0x42108421218, 0x84212100010});
constexpr StatTable44 SQR8_TABLE_44({0x1, 0x21000401000, 0x84211000021, 0xa521000, 0x42108421719, 0x311e5310e55, 0x10401008c61, 0x3210031000, 0x10c43058522, 0x74110050101, 0xf5312e97201, 0x42108421109, 0x21061501611, 0x94311002961, 0xf59421000, 0x52d4b56923a, 0x201e5300e54, 0x71501c8fe71, 0xb6002131043, 0xb5e4c168432, 0xb320f5619ae, 0xf5000f97201, 0x10c43118422, 0x24010451100, 0x942113d4330, 0x8421a521001, 0x6310e130719, 0xf72fc731c6c, 0x4ae739631, 0x70008417e4d, 0x20ca6358b77, 0x64110094a51, 0xd4002e97200, 0xf7f5a13853a, 0xb12664417f6, 0x843322d6860, 0xf7c4100194d, 0x7382d17842b, 0xf67fd71a10c, 0x6efcca4b731, 0xf4e5901ea2d, 0xcb8278a46, 0xa32050401ee, 0xb7218bb6518});
constexpr StatTable44 SQR16_TABLE_44({0x1, 0xc6cdb660138, 0x13de5a69a7b, 0x80bcafe7981, 0x60eb6f976d1, 0x677fbef6cce, 0x1549bb4cdec, 0x3b1ddf6859, 0xc01b8da28a6, 0xf3e11efbf8c, 0xd3e6faf8ee3, 0xa3dbc5712c8, 0x72361d7ca84, 0xe59e509337d, 0x15fca12a6f4, 0x33ce445498c, 0x44406de91fb, 0x9784b690571, 0xb0fb81753af, 0xb53a7c2c977, 0x34fbd3dba9b, 0xc758c22e647, 0xd5ff69aa469, 0x41e6d42b47d, 0xa4d1a3d02e7, 0x365db54ae9f, 0xd2293b8770b, 0xf1bf95c7746, 0x337fbe1d950, 0x726879e26a7, 0xa4be5ec2171, 0x7080da9df82, 0x7560017ce2, 0xd03997e34ae, 0x27ad4309a78, 0xb7b0ead892b, 0xf45bedb915d, 0xc4f0e25a52c, 0xe774a9d7fe8, 0xece6c1d7a26, 0xf20ea9ab655, 0x159bb624dc2, 0x12f2780b45f, 0x840cc52f19d});
constexpr StatTable44 QRT_TABLE_44({0xf05334f4f6e, 0x4002016, 0x4002014, 0xf04350e6246, 0x4002010, 0x4935b379a26, 0xf04350e624e, 0xf84250c228e, 0x4002000, 0xf04300e521e, 0x4935b379a06, 0xb966838dd48, 0xf04350e620e, 0xf7b8b80feda, 0xf84250c220e, 0xf972e097d5e, 0x4002100, 0x8000020000, 0xf04300e501e, 0x430025000, 0x4935b379e06, 0xf976a09dc5e, 0xb966838d548, 0xf84218c029a, 0xf04350e720e, 0x4925f36bf06, 0xf7b8b80deda, 0xb047d3ee758, 0xf84250c620e, 0xf80350e720e, 0xf972e09fd5e, 0x8091825284, 0x4012100, 0x9015063210, 0x8000000000, 0xff31a028c5e, 0xf04300a501e, 0x44340b7100, 0x4300a5000, 0, 0x4935b279e06, 0xa976b2dce18, 0xf976a29dc5e, 0x8935b279e18});
typedef Field<uint64_t, 44, 33, StatTable44, &SQR_TABLE_44, &SQR2_TABLE_44, &SQR4_TABLE_44, &SQR8_TABLE_44, &SQR16_TABLE_44, &QRT_TABLE_44, IdTrans, &ID_TRANS, &ID_TRANS> Field44;
typedef FieldTri<uint64_t, 44, 5, RecLinTrans<uint64_t, 6, 6, 6, 6, 5, 5, 5, 5>, &SQR_TABLE_44, &SQR2_TABLE_44, &SQR4_TABLE_44, &SQR8_TABLE_44, &SQR16_TABLE_44, &QRT_TABLE_44, IdTrans, &ID_TRANS, &ID_TRANS> FieldTri44;
#endif

#ifndef DISABLE_FIELD_45
// 45 bit field
typedef RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 5, 5, 5> StatTable45;
constexpr StatTable45 SQR_TABLE_45({0x1, 0x4, 0x10, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x100000, 0x400000, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x100000000, 0x400000000, 0x1000000000, 0x4000000000, 0x10000000000, 0x40000000000, 0x100000000000, 0x36, 0xd8, 0x360, 0xd80, 0x3600, 0xd800, 0x36000, 0xd8000, 0x360000, 0xd80000, 0x3600000, 0xd800000, 0x36000000, 0xd8000000, 0x360000000, 0xd80000000, 0x3600000000, 0xd800000000, 0x36000000000, 0xd8000000000, 0x16000000001b, 0x18000000005a});
constexpr StatTable45 SQR2_TABLE_45({0x1, 0x10, 0x100, 0x1000, 0x10000, 0x100000, 0x1000000, 0x10000000, 0x100000000, 0x1000000000, 0x10000000000, 0x100000000000, 0xd8, 0xd80, 0xd800, 0xd8000, 0xd80000, 0xd800000, 0xd8000000, 0xd80000000, 0xd800000000, 0xd8000000000, 0x18000000005a, 0x514, 0x5140, 0x51400, 0x514000, 0x5140000, 0x51400000, 0x514000000, 0x5140000000, 0x51400000000, 0x114000000036, 0x1400000003b8, 0x3b6e, 0x3b6e0, 0x3b6e00, 0x3b6e000, 0x3b6e0000, 0x3b6e00000, 0x3b6e000000, 0x3b6e0000000, 0x1b6e0000001b, 0x16e00000011f, 0xe0000001105});
constexpr StatTable45 SQR4_TABLE_45({0x1, 0x10000, 0x100000000, 0xd8, 0xd80000, 0xd800000000, 0x5140, 0x51400000, 0x114000000036, 0x3b6e00, 0x3b6e000000, 0xe0000001105, 0x11011000, 0x110110000000, 0x1000000d58d8, 0xd58d58000, 0x18d58000054e, 0x5451454, 0x54514540000, 0x145400038db8, 0x38db6d8e0, 0xdb6d8e00104, 0x18e00101000a, 0x10100010100, 0x10100d8d8, 0x100d8d800d8, 0x18d800d8d85a, 0xd8d8511140, 0x18511140511a, 0x114051117b58, 0x11117b556e36, 0x1b556e3b5575, 0xe3b557f1015, 0x157f1011011e, 0x101101101c48, 0x1101c458d58, 0x1c458d58d580, 0xd58d58815d4, 0x158815d1451a, 0x15d1451452c0, 0x51452ce6f6e, 0x12ce6f6db6d6, 0xf6db6da6e3d, 0x16da6e39e00f, 0xe39e00000dd});
constexpr StatTable45 SQR8_TABLE_45({0x1, 0x18d58000054e, 0xe3b557f1015, 0x51985198, 0xdb68cb55452, 0x1d0cc1d84f02, 0x140110c19ae, 0x11a16a14e7fe, 0x1e7ca7c62aa9, 0xe0eae26629b, 0x12182bee80ab, 0x1a38a68f28d3, 0x8581419c8c, 0x1d47f6f12ebc, 0x19fd34c3806e, 0x12ddba59f3cd, 0x10fa07f12a0e, 0x1d93eb544486, 0x1cf42cd119be, 0x1ff32d4b62c3, 0xf34ae031191, 0xada837715bf, 0xd368a753f92, 0x2ba87b17a03, 0x10374c3e4088, 0x1a6f539c11bd, 0x16548a5473c7, 0x1eb70011a8c9, 0x1ee5435ba1a3, 0x1173c0537680, 0xa1a3668dd6b, 0x119faad25e8, 0xd3909240e00, 0x1b560d018881, 0x127ecb9095ed, 0x306b507e701, 0x12b934c21ea3, 0x1a9d258c5b8b, 0x10452fbf0b1c, 0xae92fee120d, 0x183eb4b419fa, 0xc24d2391313, 0x4e6c4746f6, 0x2815fe7c395, 0xe4ab383747f});
constexpr StatTable45 SQR16_TABLE_45({0x1, 0x14af92df932, 0x484e0190bdc, 0xda69889e16e, 0xcf70dfdb150, 0x18c6743571a8, 0x1b2c3ad7fa79, 0x5f0cbe204f6, 0xee973392a75, 0x3e86ef79673, 0xb2a9bef7181, 0x19b5347ff116, 0x1cae0ec79856, 0x69093f18f81, 0x1964382be09a, 0x92c894b073e, 0x1d99d2922eb2, 0x647905ad0eb, 0x1695971acdd3, 0x8f3292bc8c4, 0x1ee4057ad94, 0x17f02dc60e01, 0x1bb8e05ab4d5, 0x14de5d2a05d6, 0x13a019a02983, 0xcd7097c3616, 0x1bd798639b8f, 0x1cf0ca5ac7b2, 0xa93b983cf05, 0x159a955a2aa8, 0x69e5ba33397, 0x3a6b3392237, 0x26aeab71e13, 0x26fe04d38b9, 0x1fa9df0e8c45, 0x104e85c234b0, 0x1792853f8767, 0x81573b15f20, 0x127d6bfb06d3, 0x8110e6957e8, 0x11f59cbcc110, 0xad68264cad8, 0x61438575b35, 0x56e4446dc, 0x1cc9cb28b150});
constexpr StatTable45 QRT_TABLE_45({0xede34e3e0fc, 0x1554148191aa, 0x1554148191a8, 0x1767be1dc4a6, 0x1554148191ac, 0x26bd4931492, 0x1767be1dc4ae, 0x233ab9c454a, 0x1554148191bc, 0x16939e8bb3dc, 0x26bd49314b2, 0x3c6ca8bac52, 0x1767be1dc4ee, 0x16caa5054c16, 0x233ab9c45ca, 0x14a1649628bc, 0x1554148190bc, 0x3c382881252, 0x16939e8bb1dc, 0x3c7ca0aa160, 0x26bd49310b2, 0x27f40158000, 0x3c6ca8ba452, 0x173fc092853c, 0x1767be1dd4ee, 0x16cbe284f25c, 0x16caa5056c16, 0x155559002f96, 0x233ab9c05ca, 0x26eb8908b32, 0x14a16496a8bc, 0x15440885333c, 0x1554148090bc, 0x17d60702e0, 0x3c3828a1252, 0x54548d10b2, 0x16939e8fb1dc, 0x3ac1e81b1d2, 0x3c7ca02a160, 0x166bd48310bc, 0x26bd48310b2, 0, 0x27f40358000, 0x10000000000e, 0x3c6cacba452});
typedef Field<uint64_t, 45, 27, StatTable45, &SQR_TABLE_45, &SQR2_TABLE_45, &SQR4_TABLE_45, &SQR8_TABLE_45, &SQR16_TABLE_45, &QRT_TABLE_45, IdTrans, &ID_TRANS, &ID_TRANS> Field45;
#endif

#ifndef DISABLE_FIELD_46
// 46 bit field
typedef RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 6, 5, 5> StatTableTRI46;
constexpr StatTableTRI46 SQR_TABLE_TRI46({0x1, 0x4, 0x10, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x100000, 0x400000, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x100000000, 0x400000000, 0x1000000000, 0x4000000000, 0x10000000000, 0x40000000000, 0x100000000000, 0x3, 0xc, 0x30, 0xc0, 0x300, 0xc00, 0x3000, 0xc000, 0x30000, 0xc0000, 0x300000, 0xc00000, 0x3000000, 0xc000000, 0x30000000, 0xc0000000, 0x300000000, 0xc00000000, 0x3000000000, 0xc000000000, 0x30000000000, 0xc0000000000, 0x300000000000});
constexpr StatTableTRI46 SQR2_TABLE_TRI46({0x1, 0x10, 0x100, 0x1000, 0x10000, 0x100000, 0x1000000, 0x10000000, 0x100000000, 0x1000000000, 0x10000000000, 0x100000000000, 0xc, 0xc0, 0xc00, 0xc000, 0xc0000, 0xc00000, 0xc000000, 0xc0000000, 0xc00000000, 0xc000000000, 0xc0000000000, 0x5, 0x50, 0x500, 0x5000, 0x50000, 0x500000, 0x5000000, 0x50000000, 0x500000000, 0x5000000000, 0x50000000000, 0x100000000003, 0x3c, 0x3c0, 0x3c00, 0x3c000, 0x3c0000, 0x3c00000, 0x3c000000, 0x3c0000000, 0x3c00000000, 0x3c000000000, 0x3c0000000000});
constexpr StatTableTRI46 SQR4_TABLE_TRI46({0x1, 0x10000, 0x100000000, 0xc, 0xc0000, 0xc00000000, 0x50, 0x500000, 0x5000000000, 0x3c0, 0x3c00000, 0x3c000000000, 0x1100, 0x11000000, 0x110000000000, 0xcc00, 0xcc000000, 0xc0000000005, 0x55000, 0x550000000, 0x10000000003f, 0x3fc000, 0x3fc0000000, 0x101, 0x1010000, 0x10100000000, 0xc0c, 0xc0c0000, 0xc0c00000000, 0x5050, 0x50500000, 0x105000000003, 0x3c3c0, 0x3c3c00000, 0x3c000000011, 0x111100, 0x1111000000, 0x1100000000cc, 0xcccc00, 0xcccc000000, 0xc0000000555, 0x5555000, 0x55550000000, 0x100000003fff, 0x3fffc000, 0x3fffc0000000});
constexpr StatTableTRI46 SQR8_TABLE_TRI46({0x1, 0xcc000000, 0x3c3c0, 0x10000000c, 0x550055000, 0x3c000111111, 0xc0050, 0x103fc000003f, 0x1111cccc00, 0x5000500390, 0x13ec1010101, 0xcccc9999955, 0x5000001100, 0xc0c01010000, 0xc003fffc555, 0x12c003c0cc00, 0x10505c5c0c0f, 0x155450003ffe, 0x1100cc054100, 0xfcc5053c3d1, 0xd3ff2c00d, 0xd059c3a5c3a, 0x2828282d21e, 0x5000000001, 0xcd010000, 0xc000003c695, 0x3c103c0000c, 0x55c095c0c, 0x169550112eee, 0x1100000c1150, 0x1c339050003f, 0x112e320c01, 0x1d50cc50cf95, 0x116d5292c2c2, 0xcccc9959954, 0x1050cc00113f, 0xc1d1002c3c0, 0xc013fafc509, 0x12fa93c59d01, 0x135c9081d11e, 0x150453cc3fae, 0x13f0d044d33, 0x688119f0a84, 0x39d2d62d29d, 0x3751370167, 0x24e4e4e4b4b4});
constexpr StatTableTRI46 SQR16_TABLE_TRI46({0x1, 0x1fe6e1ab503e, 0xbbae1f3f55b, 0x1d51cc530c59, 0x163a6a22e14a, 0x5847feb7f81, 0x1ec9cc5fd281, 0xf6cc7b80c70, 0x8f46b31e374, 0xc13bf2ed37d, 0x148a1595bffe, 0x581ad245849, 0x1ea6920b83c1, 0x9d9a8355c7d, 0x6bcf393d5ff, 0x1d4e245085c0, 0x602a8c5e62c, 0x1922dd69197f, 0x7945d3a2aad, 0xf82a823f768, 0xdd24665599b, 0x13b43f6a29d, 0x4df114f238d, 0x1ee783c75ec0, 0xfb670f65c31, 0xf855dc973d2, 0x61ede5f2651, 0x6c1a1266403, 0x1f66ed2a96a, 0xbbbdf683148, 0x1ecc83e160c0, 0x1a2778c4bc0c, 0x10e154273753, 0x1704f8873c23, 0x1b4d3172da99, 0x2b3be805044, 0x5bb08848b9d, 0x1967d2b99be5, 0x7fa55262740, 0xe761a27cc28, 0x17dedd7181b5, 0x155b0344714a, 0x15187b38816e, 0xc5a679b5300, 0x1096cbf94c5d, 0x3f6b3cc122da});
constexpr StatTableTRI46 QRT_TABLE_TRI46({0x211c4fd486ba, 0x100104a, 0x1001048, 0x104d0492d4, 0x100104c, 0x20005040c820, 0x104d0492dc, 0x40008080, 0x100105c, 0x24835068ce00, 0x20005040c800, 0x200000400800, 0x104d04929c, 0x100904325c, 0x40008000, 0x25da9e77daf0, 0x100115c, 0x1184e1696f0, 0x24835068cc00, 0x24825169dd5c, 0x20005040cc00, 0x3ea3241c60c0, 0x200000400000, 0x211c4e5496f0, 0x104d04829c, 0x20005340d86c, 0x100904125c, 0x24835968de5c, 0x4000c000, 0x6400a0c0, 0x25da9e775af0, 0x118cf1687ac, 0x101115c, 0x1ea1745cacc0, 0x1184e1496f0, 0x20181e445af0, 0x2483506ccc00, 0x20240060c0, 0x24825161dd5c, 0x1e21755dbd9c, 0x20005050cc00, 0x26a3746cacc0, 0x3ea3243c60c0, 0xea3243c60c0, 0x200000000000, 0});
typedef FieldTri<uint64_t, 46, 1, StatTableTRI46, &SQR_TABLE_TRI46, &SQR2_TABLE_TRI46, &SQR4_TABLE_TRI46, &SQR8_TABLE_TRI46, &SQR16_TABLE_TRI46, &QRT_TABLE_TRI46, IdTrans, &ID_TRANS, &ID_TRANS> FieldTri46;
#endif

#ifndef DISABLE_FIELD_47
// 47 bit field
typedef RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 6, 6, 5> StatTable47;
constexpr StatTable47 SQR_TABLE_47({0x1, 0x4, 0x10, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x100000, 0x400000, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x100000000, 0x400000000, 0x1000000000, 0x4000000000, 0x10000000000, 0x40000000000, 0x100000000000, 0x400000000000, 0x42, 0x108, 0x420, 0x1080, 0x4200, 0x10800, 0x42000, 0x108000, 0x420000, 0x1080000, 0x4200000, 0x10800000, 0x42000000, 0x108000000, 0x420000000, 0x1080000000, 0x4200000000, 0x10800000000, 0x42000000000, 0x108000000000, 0x420000000000, 0x80000000042, 0x200000000108});
constexpr StatTable47 SQR2_TABLE_47({0x1, 0x10, 0x100, 0x1000, 0x10000, 0x100000, 0x1000000, 0x10000000, 0x100000000, 0x1000000000, 0x10000000000, 0x100000000000, 0x42, 0x420, 0x4200, 0x42000, 0x420000, 0x4200000, 0x42000000, 0x420000000, 0x4200000000, 0x42000000000, 0x420000000000, 0x200000000108, 0x1004, 0x10040, 0x100400, 0x1004000, 0x10040000, 0x100400000, 0x1004000000, 0x10040000000, 0x100400000000, 0x4000000042, 0x40000000420, 0x400000004200, 0x42108, 0x421080, 0x4210800, 0x42108000, 0x421080000, 0x4210800000, 0x42108000000, 0x421080000000, 0x210800000108, 0x108000001004, 0x80000010002});
constexpr StatTable47 SQR4_TABLE_47({0x1, 0x10000, 0x100000000, 0x42, 0x420000, 0x4200000000, 0x1004, 0x10040000, 0x100400000000, 0x42108, 0x421080000, 0x210800000108, 0x1000010, 0x10000100000, 0x1000004200, 0x42000420, 0x420004200000, 0x42000100400, 0x1004010040, 0x40100400420, 0x4004210842, 0x42108421080, 0x84210810002, 0x108100000004, 0x142, 0x1420000, 0x14200000000, 0x5204, 0x52040000, 0x520400000000, 0x142508, 0x1425080000, 0x250800000528, 0x5210810, 0x52108100000, 0x81000014202, 0x142001420, 0x420014200042, 0x142000520400, 0x5204052040, 0x40520401424, 0x20401425094a, 0x142509425080, 0x9425085210a, 0x508521084204, 0x21084210804a, 0x421080420010});
constexpr StatTable47 SQR8_TABLE_47({0x1, 0x420004200000, 0x250800000528, 0x11004, 0xc6210910402, 0x142005730c10, 0x101000010, 0x1056050040, 0x55a429184204, 0x111404110002, 0x532408500562, 0x3d196251d62e, 0x420142, 0x44524611c66, 0x1142047728, 0x46205e2508, 0x67339c2519da, 0x5661384b5880, 0x434346200424, 0x392938cb55aa, 0x724b0c31058c, 0x7f4f1cf703fc, 0x4fe303d32d1e, 0x75de250d676c, 0x100400011004, 0xc6210910540, 0x102525331834, 0x1101046318, 0x1057532548, 0x37f4bd7f4b5e, 0x115021380840, 0x52674a501142, 0x297a4a1b86ae, 0x666b0c2010ca, 0x776839031b88, 0x4b5316a05622, 0x254e215e2030, 0x6733ce2009de, 0xa8609d21e86, 0x567347420874, 0x6e0d31db55ba, 0x4357182485c4, 0x2afb35ef02ba, 0x5af227961c30, 0x64faad1b0116, 0x2d5d2527ef40, 0x6e27e3bc1978});
constexpr StatTable47 SQR16_TABLE_47({0x1, 0x421ac25c8774, 0x30581389b510, 0x423b9c671db0, 0xa4537914208, 0x38f9be0dbf38, 0x351c5a8b92a8, 0xc38b9920da2, 0x508d34674f2a, 0x1f8c359a6b76, 0x5ac4bf86daaa, 0x51d6a6616df2, 0xe2717a0378a, 0x13353e783e6e, 0x55a55ac09ec6, 0x3f17cde43402, 0x760584b64b6c, 0x6acbecc99a02, 0x16be80e45b76, 0x2d5069e0005a, 0x3388f5759aa6, 0x2f98f891f4e2, 0x7657f368d924, 0x48f81e34f5b0, 0x51a9087f072e, 0x1de01ba9001c, 0x560b4b374bfc, 0x13f576988ff0, 0x3673cd322294, 0x595959f7c5fe, 0xbfa426eb4a4, 0x2b68fd7c02c2, 0x2a3c1437913a, 0x6e4b179fcf9e, 0x69ddf09bbdee, 0x7b91973d5e52, 0x1329cefd9514, 0x6a5f380b7ab0, 0x48e6620529c4, 0x60589a4b95b6, 0x5e4bd1d1aa34, 0x4b1ec7645cc2, 0x5cfb8785aec6, 0x34e47cf10c3a, 0x7b6c363eee10, 0x1dc52d768b32, 0x3585af9113a0});
constexpr StatTable47 QRT_TABLE_47({0, 0x1001040, 0x1001042, 0x1047043076, 0x1001046, 0x112471c241e, 0x104704307e, 0x4304e052168, 0x1001056, 0x10004000, 0x112471c243e, 0x172a09c949d6, 0x104704303e, 0x4002020, 0x4304e0521e8, 0x5400e220, 0x1001156, 0x172b08c85080, 0x10004200, 0x41200b0800, 0x112471c203e, 0x172f0cca50a0, 0x172a09c941d6, 0x7eb88a11c1d6, 0x104704203e, 0x1044042020, 0x4000020, 0x42001011156, 0x4304e0561e8, 0x172a28c95880, 0x54006220, 0x112931cc21e, 0x1011156, 0x53670f283e, 0x172b08ca5080, 0x7a80c414a03e, 0x10044200, 0x40000000000, 0x4120030800, 0x1928318801e, 0x112470c203e, 0x799283188000, 0x172f0cea50a0, 0x1eb88a91c1c8, 0x172a098941d6, 0x3ea8cc95e1f6, 0x7eb88a91c1d6});
typedef Field<uint64_t, 47, 33, StatTable47, &SQR_TABLE_47, &SQR2_TABLE_47, &SQR4_TABLE_47, &SQR8_TABLE_47, &SQR16_TABLE_47, &QRT_TABLE_47, IdTrans, &ID_TRANS, &ID_TRANS> Field47;
typedef FieldTri<uint64_t, 47, 5, RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 6, 6, 5>, &SQR_TABLE_47, &SQR2_TABLE_47, &SQR4_TABLE_47, &SQR8_TABLE_47, &SQR16_TABLE_47, &QRT_TABLE_47, IdTrans, &ID_TRANS, &ID_TRANS> FieldTri47;
#endif

#ifndef DISABLE_FIELD_48
// 48 bit field
typedef RecLinTrans<uint64_t, 6, 6, 6, 6, 6, 6, 6, 6> StatTable48;
constexpr StatTable48 SQR_TABLE_48({0x1, 0x4, 0x10, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x100000, 0x400000, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x100000000, 0x400000000, 0x1000000000, 0x4000000000, 0x10000000000, 0x40000000000, 0x100000000000, 0x400000000000, 0x2d, 0xb4, 0x2d0, 0xb40, 0x2d00, 0xb400, 0x2d000, 0xb4000, 0x2d0000, 0xb40000, 0x2d00000, 0xb400000, 0x2d000000, 0xb4000000, 0x2d0000000, 0xb40000000, 0x2d00000000, 0xb400000000, 0x2d000000000, 0xb4000000000, 0x2d0000000000, 0xb40000000000, 0xd0000000005a, 0x40000000011f});
constexpr StatTable48 SQR2_TABLE_48({0x1, 0x10, 0x100, 0x1000, 0x10000, 0x100000, 0x1000000, 0x10000000, 0x100000000, 0x1000000000, 0x10000000000, 0x100000000000, 0x2d, 0x2d0, 0x2d00, 0x2d000, 0x2d0000, 0x2d00000, 0x2d000000, 0x2d0000000, 0x2d00000000, 0x2d000000000, 0x2d0000000000, 0xd0000000005a, 0x451, 0x4510, 0x45100, 0x451000, 0x4510000, 0x45100000, 0x451000000, 0x4510000000, 0x45100000000, 0x451000000000, 0x5100000000b4, 0x100000000bd9, 0xbdbd, 0xbdbd0, 0xbdbd00, 0xbdbd000, 0xbdbd0000, 0xbdbd00000, 0xbdbd000000, 0xbdbd0000000, 0xbdbd00000000, 0xdbd00000011f, 0xbd0000001001, 0xd0000001010f});
constexpr StatTable48 SQR4_TABLE_48({0x1, 0x10000, 0x100000000, 0x2d, 0x2d0000, 0x2d00000000, 0x451, 0x4510000, 0x45100000000, 0xbdbd, 0xbdbd0000, 0xbdbd00000000, 0x101101, 0x1011010000, 0x1101000002d0, 0x2d2fd2d, 0x2d2fd2d0000, 0xfd2d0000454a, 0x45514551, 0x455145510000, 0x4551000bd0bd, 0xbd0b6d0bd, 0xd0b6d0bd011f, 0xd0bd0100011e, 0x10001010001, 0x10100012d00, 0x12d002d2d, 0x2d002d2d002d, 0x2d2d00295100, 0x2951045551, 0x5104555104e5, 0x555104ecbdb4, 0x4ecbdbd00bd, 0xbdbd00bdadbc, 0xbdadac1101, 0xadac11011001, 0x11011013c3fc, 0x1013c3fefd2d, 0xc3fefd2fd2a7, 0xfd2fd2baac36, 0xd2baac2d450b, 0xac2d45145ac2, 0x45145ad0f851, 0x5ad0f85adb64, 0xf85adb6cbd10, 0xdb6cbd0bd0a2, 0xbd0bd0bc003c, 0xd0bc002c001f});
constexpr StatTable48 SQR8_TABLE_48({0x1, 0x2d2fd2d0000, 0x4ecbdbd00bd, 0x2c0000002c, 0x47882d9b95ec, 0xb9eec2eefc90, 0xbd900450, 0x9734009bfc8f, 0x93070a51e9b7, 0xb9d0aa02ec00, 0x8630787dd0ab, 0x6b3468ab98c9, 0x4100001bc1bd, 0x3ffeec1692fd, 0x2e0cce80414a, 0x11a77fc95626, 0x7c9856084ffe, 0x2f41c702f193, 0xd260e95bfeeb, 0x3b86220b54eb, 0x5735c802071a, 0x44626fc7ba84, 0x2b9cda923f5b, 0xc57d0a962e45, 0xd1685001450b, 0x6d78400145b6, 0x9114412978c1, 0x7d9ece1f5b0c, 0xfd2419988960, 0xd12ac3eaeaa5, 0x7d67e75f441d, 0xf86c2ba5457c, 0x40db7617aa6a, 0x80ee292186, 0xbd0f8525d34c, 0xa87ce27ca699, 0xacf3315d7a6d, 0x1a289bca977, 0x92975374e0f1, 0x3fcf113826ab, 0xff4d9be19a5e, 0x1412e5091900, 0x82c721f22d43, 0x1d773380ff32, 0xfed661cca7b1, 0x308072e06846, 0xd3eb44e91aa0, 0x819a669cbb14});
constexpr StatTable48 SQR16_TABLE_48({0x1, 0x50c24311dfa9, 0xfc08c1e39482, 0xa9ff91b620a3, 0x54954a59d16c, 0xec45c0a9fb02, 0xba7004022837, 0xc1ea19828166, 0xee9a3efecffe, 0x57ed421a20bb, 0x69d387b19141, 0x9105d02d728f, 0xd2f24d4006da, 0x39195005f508, 0xd0206ff5333c, 0x8592e734a441, 0x787b36a1c435, 0x1151e6f03f85, 0xfe0429bf95ab, 0xab2b20b47651, 0x2a65fc935212, 0x7f73ae670e2e, 0x697c17f0fc4a, 0x55dc5681f013, 0xadbd09a289bc, 0x418414f64940, 0x927c737efd40, 0x38535d08fc98, 0xe811b107691c, 0x856c3bbf4cf6, 0x47e629ad3757, 0xf9c82b4b2c09, 0x64312c99e2f, 0xd4936c978dfd, 0x782ff8716675, 0xaf853e867dd7, 0x457143c1fa6d, 0x84c4dda48e91, 0xbac9aacda41e, 0x6a6e0ffb2dc1, 0xfef377f00194, 0x3a129790acc1, 0x541e49c6f92a, 0x73e821aca96d, 0x3a6f15c03f57, 0x1bf377c66f3b, 0xbff5e192fe3b, 0x346360ee74a});
constexpr StatTable48 QRT_TABLE_48({0xc00442c284f0, 0xc16b7fda410a, 0xc16b7fda4108, 0xada3b5c79fbe, 0xc16b7fda410c, 0x16f3c18d5b0, 0xada3b5c79fb6, 0x7090a381f64, 0xc16b7fda411c, 0xcafc15d179f8, 0x16f3c18d590, 0x6630880e534e, 0xada3b5c79ff6, 0xa13dd1f49826, 0x7090a381fe4, 0xb87560f6a74, 0xc16b7fda401c, 0xaaaaffff0012, 0xcafc15d17bf8, 0xaafd15f07bf6, 0x16f3c18d190, 0x60000020000e, 0x6630880e5b4e, 0xcb977fcb401c, 0xada3b5c78ff6, 0x6663420cad0, 0xa13dd1f4b826, 0xc0045fc2f41c, 0x7090a385fe4, 0x6762e24b834, 0xb87560fea74, 0xc6351fed241c, 0xc16b7fdb401c, 0x60065622ea7a, 0xaaaafffd0012, 0xdf9562bea74, 0xcafc15d57bf8, 0x6657ea057bea, 0xaafd15f87bf6, 0xa79329ddaa66, 0x16f3c08d190, 0xa39229f0aa66, 0x60000000000e, 0x175fb4468ad0, 0x6630884e5b4e, 0, 0xcb977f4b401c, 0x2630884e5b40});
typedef Field<uint64_t, 48, 45, StatTable48, &SQR_TABLE_48, &SQR2_TABLE_48, &SQR4_TABLE_48, &SQR8_TABLE_48, &SQR16_TABLE_48, &QRT_TABLE_48, IdTrans, &ID_TRANS, &ID_TRANS> Field48;
#endif
}

Sketch* ConstructClMul6Bytes(int bits, int implementation) {
    switch (bits) {
#ifndef DISABLE_FIELD_41
    case 41: return new SketchImpl<Field41>(implementation, 41);
#endif
#ifndef DISABLE_FIELD_42
    case 42: return new SketchImpl<Field42>(implementation, 42);
#endif
#ifndef DISABLE_FIELD_43
    case 43: return new SketchImpl<Field43>(implementation, 43);
#endif
#ifndef DISABLE_FIELD_44
    case 44: return new SketchImpl<Field44>(implementation, 44);
#endif
#ifndef DISABLE_FIELD_45
    case 45: return new SketchImpl<Field45>(implementation, 45);
#endif
#ifndef DISABLE_FIELD_47
    case 47: return new SketchImpl<Field47>(implementation, 47);
#endif
#ifndef DISABLE_FIELD_48
    case 48: return new SketchImpl<Field48>(implementation, 48);
#endif
    }
    return nullptr;
}

Sketch* ConstructClMulTri6Bytes(int bits, int implementation) {
    switch (bits) {
#ifndef DISABLE_FIELD_41
    case 41: return new SketchImpl<FieldTri41>(implementation, 41);
#endif
#ifndef DISABLE_FIELD_42
    case 42: return new SketchImpl<FieldTri42>(implementation, 42);
#endif
#ifndef DISABLE_FIELD_44
    case 44: return new SketchImpl<FieldTri44>(implementation, 44);
#endif
#ifndef DISABLE_FIELD_46
    case 46: return new SketchImpl<FieldTri46>(implementation, 46);
#endif
#ifndef DISABLE_FIELD_47
    case 47: return new SketchImpl<FieldTri47>(implementation, 47);
#endif
    }
    return nullptr;
}
