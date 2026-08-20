// SPDX-License-Identifier: GPL-2.0-only
/*
 * Is this machine sensitive to instruction-cache pressure?
 *
 * The theory for the board-wide slowdown is that a 16 KB L1 icache, shared by
 * the XIP kernel and by code paged from PSRAM, is thrashed by frequent
 * interrupts, so anything with a large code footprint pays to refetch. If that
 * is right, a loop spanning far more than 16 KB of code should degrade much
 * harder under interrupt load than a loop that fits in a few cache lines.
 *
 * Both loops do the same arithmetic; only their code footprint differs.
 */

#include <stdio.h>
#include <time.h>

#define noinline __attribute__((noinline))

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static noinline int tiny(int x) { return x * 3 + 1; }

static noinline int f0(int x) { return x * 1 + 0; }
static noinline int f1(int x) { return x * 1 + 1; }
static noinline int f2(int x) { return x * 3 + 2; }
static noinline int f3(int x) { return x * 3 + 3; }
static noinline int f4(int x) { return x * 5 + 4; }
static noinline int f5(int x) { return x * 5 + 5; }
static noinline int f6(int x) { return x * 7 + 6; }
static noinline int f7(int x) { return x * 7 + 7; }
static noinline int f8(int x) { return x * 9 + 8; }
static noinline int f9(int x) { return x * 9 + 9; }
static noinline int f10(int x) { return x * 11 + 10; }
static noinline int f11(int x) { return x * 11 + 11; }
static noinline int f12(int x) { return x * 13 + 12; }
static noinline int f13(int x) { return x * 13 + 13; }
static noinline int f14(int x) { return x * 15 + 14; }
static noinline int f15(int x) { return x * 15 + 15; }
static noinline int f16(int x) { return x * 17 + 16; }
static noinline int f17(int x) { return x * 17 + 17; }
static noinline int f18(int x) { return x * 19 + 18; }
static noinline int f19(int x) { return x * 19 + 19; }
static noinline int f20(int x) { return x * 21 + 20; }
static noinline int f21(int x) { return x * 21 + 21; }
static noinline int f22(int x) { return x * 23 + 22; }
static noinline int f23(int x) { return x * 23 + 23; }
static noinline int f24(int x) { return x * 25 + 24; }
static noinline int f25(int x) { return x * 25 + 25; }
static noinline int f26(int x) { return x * 27 + 26; }
static noinline int f27(int x) { return x * 27 + 27; }
static noinline int f28(int x) { return x * 29 + 28; }
static noinline int f29(int x) { return x * 29 + 29; }
static noinline int f30(int x) { return x * 31 + 30; }
static noinline int f31(int x) { return x * 31 + 31; }
static noinline int f32(int x) { return x * 33 + 32; }
static noinline int f33(int x) { return x * 33 + 33; }
static noinline int f34(int x) { return x * 35 + 34; }
static noinline int f35(int x) { return x * 35 + 35; }
static noinline int f36(int x) { return x * 37 + 36; }
static noinline int f37(int x) { return x * 37 + 37; }
static noinline int f38(int x) { return x * 39 + 38; }
static noinline int f39(int x) { return x * 39 + 39; }
static noinline int f40(int x) { return x * 41 + 40; }
static noinline int f41(int x) { return x * 41 + 41; }
static noinline int f42(int x) { return x * 43 + 42; }
static noinline int f43(int x) { return x * 43 + 43; }
static noinline int f44(int x) { return x * 45 + 44; }
static noinline int f45(int x) { return x * 45 + 45; }
static noinline int f46(int x) { return x * 47 + 46; }
static noinline int f47(int x) { return x * 47 + 47; }
static noinline int f48(int x) { return x * 49 + 48; }
static noinline int f49(int x) { return x * 49 + 49; }
static noinline int f50(int x) { return x * 51 + 50; }
static noinline int f51(int x) { return x * 51 + 51; }
static noinline int f52(int x) { return x * 53 + 52; }
static noinline int f53(int x) { return x * 53 + 53; }
static noinline int f54(int x) { return x * 55 + 54; }
static noinline int f55(int x) { return x * 55 + 55; }
static noinline int f56(int x) { return x * 57 + 56; }
static noinline int f57(int x) { return x * 57 + 57; }
static noinline int f58(int x) { return x * 59 + 58; }
static noinline int f59(int x) { return x * 59 + 59; }
static noinline int f60(int x) { return x * 61 + 60; }
static noinline int f61(int x) { return x * 61 + 61; }
static noinline int f62(int x) { return x * 63 + 62; }
static noinline int f63(int x) { return x * 63 + 63; }
static noinline int f64(int x) { return x * 65 + 64; }
static noinline int f65(int x) { return x * 65 + 65; }
static noinline int f66(int x) { return x * 67 + 66; }
static noinline int f67(int x) { return x * 67 + 67; }
static noinline int f68(int x) { return x * 69 + 68; }
static noinline int f69(int x) { return x * 69 + 69; }
static noinline int f70(int x) { return x * 71 + 70; }
static noinline int f71(int x) { return x * 71 + 71; }
static noinline int f72(int x) { return x * 73 + 72; }
static noinline int f73(int x) { return x * 73 + 73; }
static noinline int f74(int x) { return x * 75 + 74; }
static noinline int f75(int x) { return x * 75 + 75; }
static noinline int f76(int x) { return x * 77 + 76; }
static noinline int f77(int x) { return x * 77 + 77; }
static noinline int f78(int x) { return x * 79 + 78; }
static noinline int f79(int x) { return x * 79 + 79; }
static noinline int f80(int x) { return x * 81 + 80; }
static noinline int f81(int x) { return x * 81 + 81; }
static noinline int f82(int x) { return x * 83 + 82; }
static noinline int f83(int x) { return x * 83 + 83; }
static noinline int f84(int x) { return x * 85 + 84; }
static noinline int f85(int x) { return x * 85 + 85; }
static noinline int f86(int x) { return x * 87 + 86; }
static noinline int f87(int x) { return x * 87 + 87; }
static noinline int f88(int x) { return x * 89 + 88; }
static noinline int f89(int x) { return x * 89 + 89; }
static noinline int f90(int x) { return x * 91 + 90; }
static noinline int f91(int x) { return x * 91 + 91; }
static noinline int f92(int x) { return x * 93 + 92; }
static noinline int f93(int x) { return x * 93 + 93; }
static noinline int f94(int x) { return x * 95 + 94; }
static noinline int f95(int x) { return x * 95 + 95; }
static noinline int f96(int x) { return x * 97 + 96; }
static noinline int f97(int x) { return x * 97 + 97; }
static noinline int f98(int x) { return x * 99 + 98; }
static noinline int f99(int x) { return x * 99 + 99; }
static noinline int f100(int x) { return x * 101 + 100; }
static noinline int f101(int x) { return x * 101 + 101; }
static noinline int f102(int x) { return x * 103 + 102; }
static noinline int f103(int x) { return x * 103 + 103; }
static noinline int f104(int x) { return x * 105 + 104; }
static noinline int f105(int x) { return x * 105 + 105; }
static noinline int f106(int x) { return x * 107 + 106; }
static noinline int f107(int x) { return x * 107 + 107; }
static noinline int f108(int x) { return x * 109 + 108; }
static noinline int f109(int x) { return x * 109 + 109; }
static noinline int f110(int x) { return x * 111 + 110; }
static noinline int f111(int x) { return x * 111 + 111; }
static noinline int f112(int x) { return x * 113 + 112; }
static noinline int f113(int x) { return x * 113 + 113; }
static noinline int f114(int x) { return x * 115 + 114; }
static noinline int f115(int x) { return x * 115 + 115; }
static noinline int f116(int x) { return x * 117 + 116; }
static noinline int f117(int x) { return x * 117 + 117; }
static noinline int f118(int x) { return x * 119 + 118; }
static noinline int f119(int x) { return x * 119 + 119; }
static noinline int f120(int x) { return x * 121 + 120; }
static noinline int f121(int x) { return x * 121 + 121; }
static noinline int f122(int x) { return x * 123 + 122; }
static noinline int f123(int x) { return x * 123 + 123; }
static noinline int f124(int x) { return x * 125 + 124; }
static noinline int f125(int x) { return x * 125 + 125; }
static noinline int f126(int x) { return x * 127 + 126; }
static noinline int f127(int x) { return x * 127 + 127; }
static noinline int f128(int x) { return x * 129 + 128; }
static noinline int f129(int x) { return x * 129 + 129; }
static noinline int f130(int x) { return x * 131 + 130; }
static noinline int f131(int x) { return x * 131 + 131; }
static noinline int f132(int x) { return x * 133 + 132; }
static noinline int f133(int x) { return x * 133 + 133; }
static noinline int f134(int x) { return x * 135 + 134; }
static noinline int f135(int x) { return x * 135 + 135; }
static noinline int f136(int x) { return x * 137 + 136; }
static noinline int f137(int x) { return x * 137 + 137; }
static noinline int f138(int x) { return x * 139 + 138; }
static noinline int f139(int x) { return x * 139 + 139; }
static noinline int f140(int x) { return x * 141 + 140; }
static noinline int f141(int x) { return x * 141 + 141; }
static noinline int f142(int x) { return x * 143 + 142; }
static noinline int f143(int x) { return x * 143 + 143; }
static noinline int f144(int x) { return x * 145 + 144; }
static noinline int f145(int x) { return x * 145 + 145; }
static noinline int f146(int x) { return x * 147 + 146; }
static noinline int f147(int x) { return x * 147 + 147; }
static noinline int f148(int x) { return x * 149 + 148; }
static noinline int f149(int x) { return x * 149 + 149; }
static noinline int f150(int x) { return x * 151 + 150; }
static noinline int f151(int x) { return x * 151 + 151; }
static noinline int f152(int x) { return x * 153 + 152; }
static noinline int f153(int x) { return x * 153 + 153; }
static noinline int f154(int x) { return x * 155 + 154; }
static noinline int f155(int x) { return x * 155 + 155; }
static noinline int f156(int x) { return x * 157 + 156; }
static noinline int f157(int x) { return x * 157 + 157; }
static noinline int f158(int x) { return x * 159 + 158; }
static noinline int f159(int x) { return x * 159 + 159; }
static noinline int f160(int x) { return x * 161 + 160; }
static noinline int f161(int x) { return x * 161 + 161; }
static noinline int f162(int x) { return x * 163 + 162; }
static noinline int f163(int x) { return x * 163 + 163; }
static noinline int f164(int x) { return x * 165 + 164; }
static noinline int f165(int x) { return x * 165 + 165; }
static noinline int f166(int x) { return x * 167 + 166; }
static noinline int f167(int x) { return x * 167 + 167; }
static noinline int f168(int x) { return x * 169 + 168; }
static noinline int f169(int x) { return x * 169 + 169; }
static noinline int f170(int x) { return x * 171 + 170; }
static noinline int f171(int x) { return x * 171 + 171; }
static noinline int f172(int x) { return x * 173 + 172; }
static noinline int f173(int x) { return x * 173 + 173; }
static noinline int f174(int x) { return x * 175 + 174; }
static noinline int f175(int x) { return x * 175 + 175; }
static noinline int f176(int x) { return x * 177 + 176; }
static noinline int f177(int x) { return x * 177 + 177; }
static noinline int f178(int x) { return x * 179 + 178; }
static noinline int f179(int x) { return x * 179 + 179; }
static noinline int f180(int x) { return x * 181 + 180; }
static noinline int f181(int x) { return x * 181 + 181; }
static noinline int f182(int x) { return x * 183 + 182; }
static noinline int f183(int x) { return x * 183 + 183; }
static noinline int f184(int x) { return x * 185 + 184; }
static noinline int f185(int x) { return x * 185 + 185; }
static noinline int f186(int x) { return x * 187 + 186; }
static noinline int f187(int x) { return x * 187 + 187; }
static noinline int f188(int x) { return x * 189 + 188; }
static noinline int f189(int x) { return x * 189 + 189; }
static noinline int f190(int x) { return x * 191 + 190; }
static noinline int f191(int x) { return x * 191 + 191; }
static noinline int f192(int x) { return x * 193 + 192; }
static noinline int f193(int x) { return x * 193 + 193; }
static noinline int f194(int x) { return x * 195 + 194; }
static noinline int f195(int x) { return x * 195 + 195; }
static noinline int f196(int x) { return x * 197 + 196; }
static noinline int f197(int x) { return x * 197 + 197; }
static noinline int f198(int x) { return x * 199 + 198; }
static noinline int f199(int x) { return x * 199 + 199; }
static noinline int f200(int x) { return x * 201 + 200; }
static noinline int f201(int x) { return x * 201 + 201; }
static noinline int f202(int x) { return x * 203 + 202; }
static noinline int f203(int x) { return x * 203 + 203; }
static noinline int f204(int x) { return x * 205 + 204; }
static noinline int f205(int x) { return x * 205 + 205; }
static noinline int f206(int x) { return x * 207 + 206; }
static noinline int f207(int x) { return x * 207 + 207; }
static noinline int f208(int x) { return x * 209 + 208; }
static noinline int f209(int x) { return x * 209 + 209; }
static noinline int f210(int x) { return x * 211 + 210; }
static noinline int f211(int x) { return x * 211 + 211; }
static noinline int f212(int x) { return x * 213 + 212; }
static noinline int f213(int x) { return x * 213 + 213; }
static noinline int f214(int x) { return x * 215 + 214; }
static noinline int f215(int x) { return x * 215 + 215; }
static noinline int f216(int x) { return x * 217 + 216; }
static noinline int f217(int x) { return x * 217 + 217; }
static noinline int f218(int x) { return x * 219 + 218; }
static noinline int f219(int x) { return x * 219 + 219; }
static noinline int f220(int x) { return x * 221 + 220; }
static noinline int f221(int x) { return x * 221 + 221; }
static noinline int f222(int x) { return x * 223 + 222; }
static noinline int f223(int x) { return x * 223 + 223; }
static noinline int f224(int x) { return x * 225 + 224; }
static noinline int f225(int x) { return x * 225 + 225; }
static noinline int f226(int x) { return x * 227 + 226; }
static noinline int f227(int x) { return x * 227 + 227; }
static noinline int f228(int x) { return x * 229 + 228; }
static noinline int f229(int x) { return x * 229 + 229; }
static noinline int f230(int x) { return x * 231 + 230; }
static noinline int f231(int x) { return x * 231 + 231; }
static noinline int f232(int x) { return x * 233 + 232; }
static noinline int f233(int x) { return x * 233 + 233; }
static noinline int f234(int x) { return x * 235 + 234; }
static noinline int f235(int x) { return x * 235 + 235; }
static noinline int f236(int x) { return x * 237 + 236; }
static noinline int f237(int x) { return x * 237 + 237; }
static noinline int f238(int x) { return x * 239 + 238; }
static noinline int f239(int x) { return x * 239 + 239; }
static noinline int f240(int x) { return x * 241 + 240; }
static noinline int f241(int x) { return x * 241 + 241; }
static noinline int f242(int x) { return x * 243 + 242; }
static noinline int f243(int x) { return x * 243 + 243; }
static noinline int f244(int x) { return x * 245 + 244; }
static noinline int f245(int x) { return x * 245 + 245; }
static noinline int f246(int x) { return x * 247 + 246; }
static noinline int f247(int x) { return x * 247 + 247; }
static noinline int f248(int x) { return x * 249 + 248; }
static noinline int f249(int x) { return x * 249 + 249; }
static noinline int f250(int x) { return x * 251 + 250; }
static noinline int f251(int x) { return x * 251 + 251; }
static noinline int f252(int x) { return x * 253 + 252; }
static noinline int f253(int x) { return x * 253 + 253; }
static noinline int f254(int x) { return x * 255 + 254; }
static noinline int f255(int x) { return x * 255 + 255; }
static noinline int f256(int x) { return x * 257 + 256; }
static noinline int f257(int x) { return x * 257 + 257; }
static noinline int f258(int x) { return x * 259 + 258; }
static noinline int f259(int x) { return x * 259 + 259; }
static noinline int f260(int x) { return x * 261 + 260; }
static noinline int f261(int x) { return x * 261 + 261; }
static noinline int f262(int x) { return x * 263 + 262; }
static noinline int f263(int x) { return x * 263 + 263; }
static noinline int f264(int x) { return x * 265 + 264; }
static noinline int f265(int x) { return x * 265 + 265; }
static noinline int f266(int x) { return x * 267 + 266; }
static noinline int f267(int x) { return x * 267 + 267; }
static noinline int f268(int x) { return x * 269 + 268; }
static noinline int f269(int x) { return x * 269 + 269; }
static noinline int f270(int x) { return x * 271 + 270; }
static noinline int f271(int x) { return x * 271 + 271; }
static noinline int f272(int x) { return x * 273 + 272; }
static noinline int f273(int x) { return x * 273 + 273; }
static noinline int f274(int x) { return x * 275 + 274; }
static noinline int f275(int x) { return x * 275 + 275; }
static noinline int f276(int x) { return x * 277 + 276; }
static noinline int f277(int x) { return x * 277 + 277; }
static noinline int f278(int x) { return x * 279 + 278; }
static noinline int f279(int x) { return x * 279 + 279; }
static noinline int f280(int x) { return x * 281 + 280; }
static noinline int f281(int x) { return x * 281 + 281; }
static noinline int f282(int x) { return x * 283 + 282; }
static noinline int f283(int x) { return x * 283 + 283; }
static noinline int f284(int x) { return x * 285 + 284; }
static noinline int f285(int x) { return x * 285 + 285; }
static noinline int f286(int x) { return x * 287 + 286; }
static noinline int f287(int x) { return x * 287 + 287; }
static noinline int f288(int x) { return x * 289 + 288; }
static noinline int f289(int x) { return x * 289 + 289; }
static noinline int f290(int x) { return x * 291 + 290; }
static noinline int f291(int x) { return x * 291 + 291; }
static noinline int f292(int x) { return x * 293 + 292; }
static noinline int f293(int x) { return x * 293 + 293; }
static noinline int f294(int x) { return x * 295 + 294; }
static noinline int f295(int x) { return x * 295 + 295; }
static noinline int f296(int x) { return x * 297 + 296; }
static noinline int f297(int x) { return x * 297 + 297; }
static noinline int f298(int x) { return x * 299 + 298; }
static noinline int f299(int x) { return x * 299 + 299; }
static noinline int f300(int x) { return x * 301 + 300; }
static noinline int f301(int x) { return x * 301 + 301; }
static noinline int f302(int x) { return x * 303 + 302; }
static noinline int f303(int x) { return x * 303 + 303; }
static noinline int f304(int x) { return x * 305 + 304; }
static noinline int f305(int x) { return x * 305 + 305; }
static noinline int f306(int x) { return x * 307 + 306; }
static noinline int f307(int x) { return x * 307 + 307; }
static noinline int f308(int x) { return x * 309 + 308; }
static noinline int f309(int x) { return x * 309 + 309; }
static noinline int f310(int x) { return x * 311 + 310; }
static noinline int f311(int x) { return x * 311 + 311; }
static noinline int f312(int x) { return x * 313 + 312; }
static noinline int f313(int x) { return x * 313 + 313; }
static noinline int f314(int x) { return x * 315 + 314; }
static noinline int f315(int x) { return x * 315 + 315; }
static noinline int f316(int x) { return x * 317 + 316; }
static noinline int f317(int x) { return x * 317 + 317; }
static noinline int f318(int x) { return x * 319 + 318; }
static noinline int f319(int x) { return x * 319 + 319; }
static noinline int f320(int x) { return x * 321 + 320; }
static noinline int f321(int x) { return x * 321 + 321; }
static noinline int f322(int x) { return x * 323 + 322; }
static noinline int f323(int x) { return x * 323 + 323; }
static noinline int f324(int x) { return x * 325 + 324; }
static noinline int f325(int x) { return x * 325 + 325; }
static noinline int f326(int x) { return x * 327 + 326; }
static noinline int f327(int x) { return x * 327 + 327; }
static noinline int f328(int x) { return x * 329 + 328; }
static noinline int f329(int x) { return x * 329 + 329; }
static noinline int f330(int x) { return x * 331 + 330; }
static noinline int f331(int x) { return x * 331 + 331; }
static noinline int f332(int x) { return x * 333 + 332; }
static noinline int f333(int x) { return x * 333 + 333; }
static noinline int f334(int x) { return x * 335 + 334; }
static noinline int f335(int x) { return x * 335 + 335; }
static noinline int f336(int x) { return x * 337 + 336; }
static noinline int f337(int x) { return x * 337 + 337; }
static noinline int f338(int x) { return x * 339 + 338; }
static noinline int f339(int x) { return x * 339 + 339; }
static noinline int f340(int x) { return x * 341 + 340; }
static noinline int f341(int x) { return x * 341 + 341; }
static noinline int f342(int x) { return x * 343 + 342; }
static noinline int f343(int x) { return x * 343 + 343; }
static noinline int f344(int x) { return x * 345 + 344; }
static noinline int f345(int x) { return x * 345 + 345; }
static noinline int f346(int x) { return x * 347 + 346; }
static noinline int f347(int x) { return x * 347 + 347; }
static noinline int f348(int x) { return x * 349 + 348; }
static noinline int f349(int x) { return x * 349 + 349; }
static noinline int f350(int x) { return x * 351 + 350; }
static noinline int f351(int x) { return x * 351 + 351; }
static noinline int f352(int x) { return x * 353 + 352; }
static noinline int f353(int x) { return x * 353 + 353; }
static noinline int f354(int x) { return x * 355 + 354; }
static noinline int f355(int x) { return x * 355 + 355; }
static noinline int f356(int x) { return x * 357 + 356; }
static noinline int f357(int x) { return x * 357 + 357; }
static noinline int f358(int x) { return x * 359 + 358; }
static noinline int f359(int x) { return x * 359 + 359; }
static noinline int f360(int x) { return x * 361 + 360; }
static noinline int f361(int x) { return x * 361 + 361; }
static noinline int f362(int x) { return x * 363 + 362; }
static noinline int f363(int x) { return x * 363 + 363; }
static noinline int f364(int x) { return x * 365 + 364; }
static noinline int f365(int x) { return x * 365 + 365; }
static noinline int f366(int x) { return x * 367 + 366; }
static noinline int f367(int x) { return x * 367 + 367; }
static noinline int f368(int x) { return x * 369 + 368; }
static noinline int f369(int x) { return x * 369 + 369; }
static noinline int f370(int x) { return x * 371 + 370; }
static noinline int f371(int x) { return x * 371 + 371; }
static noinline int f372(int x) { return x * 373 + 372; }
static noinline int f373(int x) { return x * 373 + 373; }
static noinline int f374(int x) { return x * 375 + 374; }
static noinline int f375(int x) { return x * 375 + 375; }
static noinline int f376(int x) { return x * 377 + 376; }
static noinline int f377(int x) { return x * 377 + 377; }
static noinline int f378(int x) { return x * 379 + 378; }
static noinline int f379(int x) { return x * 379 + 379; }
static noinline int f380(int x) { return x * 381 + 380; }
static noinline int f381(int x) { return x * 381 + 381; }
static noinline int f382(int x) { return x * 383 + 382; }
static noinline int f383(int x) { return x * 383 + 383; }
static noinline int f384(int x) { return x * 385 + 384; }
static noinline int f385(int x) { return x * 385 + 385; }
static noinline int f386(int x) { return x * 387 + 386; }
static noinline int f387(int x) { return x * 387 + 387; }
static noinline int f388(int x) { return x * 389 + 388; }
static noinline int f389(int x) { return x * 389 + 389; }
static noinline int f390(int x) { return x * 391 + 390; }
static noinline int f391(int x) { return x * 391 + 391; }
static noinline int f392(int x) { return x * 393 + 392; }
static noinline int f393(int x) { return x * 393 + 393; }
static noinline int f394(int x) { return x * 395 + 394; }
static noinline int f395(int x) { return x * 395 + 395; }
static noinline int f396(int x) { return x * 397 + 396; }
static noinline int f397(int x) { return x * 397 + 397; }
static noinline int f398(int x) { return x * 399 + 398; }
static noinline int f399(int x) { return x * 399 + 399; }

int main(void)
{
	const long n = 20000;
	double t0, t1, small, large;
	int acc = 1;

	t0 = now();
	for (long i = 0; i < n; i++)
		for (int r = 0; r < 400; r++)
			acc = tiny(acc);
	t1 = now();
	small = (t1 - t0) * 1e9 / (n * 400.0);

	t0 = now();
	for (long i = 0; i < n; i++) {
		acc = f0(acc);
		acc = f1(acc);
		acc = f2(acc);
		acc = f3(acc);
		acc = f4(acc);
		acc = f5(acc);
		acc = f6(acc);
		acc = f7(acc);
		acc = f8(acc);
		acc = f9(acc);
		acc = f10(acc);
		acc = f11(acc);
		acc = f12(acc);
		acc = f13(acc);
		acc = f14(acc);
		acc = f15(acc);
		acc = f16(acc);
		acc = f17(acc);
		acc = f18(acc);
		acc = f19(acc);
		acc = f20(acc);
		acc = f21(acc);
		acc = f22(acc);
		acc = f23(acc);
		acc = f24(acc);
		acc = f25(acc);
		acc = f26(acc);
		acc = f27(acc);
		acc = f28(acc);
		acc = f29(acc);
		acc = f30(acc);
		acc = f31(acc);
		acc = f32(acc);
		acc = f33(acc);
		acc = f34(acc);
		acc = f35(acc);
		acc = f36(acc);
		acc = f37(acc);
		acc = f38(acc);
		acc = f39(acc);
		acc = f40(acc);
		acc = f41(acc);
		acc = f42(acc);
		acc = f43(acc);
		acc = f44(acc);
		acc = f45(acc);
		acc = f46(acc);
		acc = f47(acc);
		acc = f48(acc);
		acc = f49(acc);
		acc = f50(acc);
		acc = f51(acc);
		acc = f52(acc);
		acc = f53(acc);
		acc = f54(acc);
		acc = f55(acc);
		acc = f56(acc);
		acc = f57(acc);
		acc = f58(acc);
		acc = f59(acc);
		acc = f60(acc);
		acc = f61(acc);
		acc = f62(acc);
		acc = f63(acc);
		acc = f64(acc);
		acc = f65(acc);
		acc = f66(acc);
		acc = f67(acc);
		acc = f68(acc);
		acc = f69(acc);
		acc = f70(acc);
		acc = f71(acc);
		acc = f72(acc);
		acc = f73(acc);
		acc = f74(acc);
		acc = f75(acc);
		acc = f76(acc);
		acc = f77(acc);
		acc = f78(acc);
		acc = f79(acc);
		acc = f80(acc);
		acc = f81(acc);
		acc = f82(acc);
		acc = f83(acc);
		acc = f84(acc);
		acc = f85(acc);
		acc = f86(acc);
		acc = f87(acc);
		acc = f88(acc);
		acc = f89(acc);
		acc = f90(acc);
		acc = f91(acc);
		acc = f92(acc);
		acc = f93(acc);
		acc = f94(acc);
		acc = f95(acc);
		acc = f96(acc);
		acc = f97(acc);
		acc = f98(acc);
		acc = f99(acc);
		acc = f100(acc);
		acc = f101(acc);
		acc = f102(acc);
		acc = f103(acc);
		acc = f104(acc);
		acc = f105(acc);
		acc = f106(acc);
		acc = f107(acc);
		acc = f108(acc);
		acc = f109(acc);
		acc = f110(acc);
		acc = f111(acc);
		acc = f112(acc);
		acc = f113(acc);
		acc = f114(acc);
		acc = f115(acc);
		acc = f116(acc);
		acc = f117(acc);
		acc = f118(acc);
		acc = f119(acc);
		acc = f120(acc);
		acc = f121(acc);
		acc = f122(acc);
		acc = f123(acc);
		acc = f124(acc);
		acc = f125(acc);
		acc = f126(acc);
		acc = f127(acc);
		acc = f128(acc);
		acc = f129(acc);
		acc = f130(acc);
		acc = f131(acc);
		acc = f132(acc);
		acc = f133(acc);
		acc = f134(acc);
		acc = f135(acc);
		acc = f136(acc);
		acc = f137(acc);
		acc = f138(acc);
		acc = f139(acc);
		acc = f140(acc);
		acc = f141(acc);
		acc = f142(acc);
		acc = f143(acc);
		acc = f144(acc);
		acc = f145(acc);
		acc = f146(acc);
		acc = f147(acc);
		acc = f148(acc);
		acc = f149(acc);
		acc = f150(acc);
		acc = f151(acc);
		acc = f152(acc);
		acc = f153(acc);
		acc = f154(acc);
		acc = f155(acc);
		acc = f156(acc);
		acc = f157(acc);
		acc = f158(acc);
		acc = f159(acc);
		acc = f160(acc);
		acc = f161(acc);
		acc = f162(acc);
		acc = f163(acc);
		acc = f164(acc);
		acc = f165(acc);
		acc = f166(acc);
		acc = f167(acc);
		acc = f168(acc);
		acc = f169(acc);
		acc = f170(acc);
		acc = f171(acc);
		acc = f172(acc);
		acc = f173(acc);
		acc = f174(acc);
		acc = f175(acc);
		acc = f176(acc);
		acc = f177(acc);
		acc = f178(acc);
		acc = f179(acc);
		acc = f180(acc);
		acc = f181(acc);
		acc = f182(acc);
		acc = f183(acc);
		acc = f184(acc);
		acc = f185(acc);
		acc = f186(acc);
		acc = f187(acc);
		acc = f188(acc);
		acc = f189(acc);
		acc = f190(acc);
		acc = f191(acc);
		acc = f192(acc);
		acc = f193(acc);
		acc = f194(acc);
		acc = f195(acc);
		acc = f196(acc);
		acc = f197(acc);
		acc = f198(acc);
		acc = f199(acc);
		acc = f200(acc);
		acc = f201(acc);
		acc = f202(acc);
		acc = f203(acc);
		acc = f204(acc);
		acc = f205(acc);
		acc = f206(acc);
		acc = f207(acc);
		acc = f208(acc);
		acc = f209(acc);
		acc = f210(acc);
		acc = f211(acc);
		acc = f212(acc);
		acc = f213(acc);
		acc = f214(acc);
		acc = f215(acc);
		acc = f216(acc);
		acc = f217(acc);
		acc = f218(acc);
		acc = f219(acc);
		acc = f220(acc);
		acc = f221(acc);
		acc = f222(acc);
		acc = f223(acc);
		acc = f224(acc);
		acc = f225(acc);
		acc = f226(acc);
		acc = f227(acc);
		acc = f228(acc);
		acc = f229(acc);
		acc = f230(acc);
		acc = f231(acc);
		acc = f232(acc);
		acc = f233(acc);
		acc = f234(acc);
		acc = f235(acc);
		acc = f236(acc);
		acc = f237(acc);
		acc = f238(acc);
		acc = f239(acc);
		acc = f240(acc);
		acc = f241(acc);
		acc = f242(acc);
		acc = f243(acc);
		acc = f244(acc);
		acc = f245(acc);
		acc = f246(acc);
		acc = f247(acc);
		acc = f248(acc);
		acc = f249(acc);
		acc = f250(acc);
		acc = f251(acc);
		acc = f252(acc);
		acc = f253(acc);
		acc = f254(acc);
		acc = f255(acc);
		acc = f256(acc);
		acc = f257(acc);
		acc = f258(acc);
		acc = f259(acc);
		acc = f260(acc);
		acc = f261(acc);
		acc = f262(acc);
		acc = f263(acc);
		acc = f264(acc);
		acc = f265(acc);
		acc = f266(acc);
		acc = f267(acc);
		acc = f268(acc);
		acc = f269(acc);
		acc = f270(acc);
		acc = f271(acc);
		acc = f272(acc);
		acc = f273(acc);
		acc = f274(acc);
		acc = f275(acc);
		acc = f276(acc);
		acc = f277(acc);
		acc = f278(acc);
		acc = f279(acc);
		acc = f280(acc);
		acc = f281(acc);
		acc = f282(acc);
		acc = f283(acc);
		acc = f284(acc);
		acc = f285(acc);
		acc = f286(acc);
		acc = f287(acc);
		acc = f288(acc);
		acc = f289(acc);
		acc = f290(acc);
		acc = f291(acc);
		acc = f292(acc);
		acc = f293(acc);
		acc = f294(acc);
		acc = f295(acc);
		acc = f296(acc);
		acc = f297(acc);
		acc = f298(acc);
		acc = f299(acc);
		acc = f300(acc);
		acc = f301(acc);
		acc = f302(acc);
		acc = f303(acc);
		acc = f304(acc);
		acc = f305(acc);
		acc = f306(acc);
		acc = f307(acc);
		acc = f308(acc);
		acc = f309(acc);
		acc = f310(acc);
		acc = f311(acc);
		acc = f312(acc);
		acc = f313(acc);
		acc = f314(acc);
		acc = f315(acc);
		acc = f316(acc);
		acc = f317(acc);
		acc = f318(acc);
		acc = f319(acc);
		acc = f320(acc);
		acc = f321(acc);
		acc = f322(acc);
		acc = f323(acc);
		acc = f324(acc);
		acc = f325(acc);
		acc = f326(acc);
		acc = f327(acc);
		acc = f328(acc);
		acc = f329(acc);
		acc = f330(acc);
		acc = f331(acc);
		acc = f332(acc);
		acc = f333(acc);
		acc = f334(acc);
		acc = f335(acc);
		acc = f336(acc);
		acc = f337(acc);
		acc = f338(acc);
		acc = f339(acc);
		acc = f340(acc);
		acc = f341(acc);
		acc = f342(acc);
		acc = f343(acc);
		acc = f344(acc);
		acc = f345(acc);
		acc = f346(acc);
		acc = f347(acc);
		acc = f348(acc);
		acc = f349(acc);
		acc = f350(acc);
		acc = f351(acc);
		acc = f352(acc);
		acc = f353(acc);
		acc = f354(acc);
		acc = f355(acc);
		acc = f356(acc);
		acc = f357(acc);
		acc = f358(acc);
		acc = f359(acc);
		acc = f360(acc);
		acc = f361(acc);
		acc = f362(acc);
		acc = f363(acc);
		acc = f364(acc);
		acc = f365(acc);
		acc = f366(acc);
		acc = f367(acc);
		acc = f368(acc);
		acc = f369(acc);
		acc = f370(acc);
		acc = f371(acc);
		acc = f372(acc);
		acc = f373(acc);
		acc = f374(acc);
		acc = f375(acc);
		acc = f376(acc);
		acc = f377(acc);
		acc = f378(acc);
		acc = f379(acc);
		acc = f380(acc);
		acc = f381(acc);
		acc = f382(acc);
		acc = f383(acc);
		acc = f384(acc);
		acc = f385(acc);
		acc = f386(acc);
		acc = f387(acc);
		acc = f388(acc);
		acc = f389(acc);
		acc = f390(acc);
		acc = f391(acc);
		acc = f392(acc);
		acc = f393(acc);
		acc = f394(acc);
		acc = f395(acc);
		acc = f396(acc);
		acc = f397(acc);
		acc = f398(acc);
		acc = f399(acc);
	}
	t1 = now();
	large = (t1 - t0) * 1e9 / (n * 400.0);

	printf("small footprint: %7.1f ns per call\n", small);
	printf("large footprint: %7.1f ns per call  (400 functions)\n", large);
	printf("ratio large/small: %.2f  (acc %d)\n", large / small, acc);
	return 0;
}
