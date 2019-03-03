// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// TODO: Test and maybe fix: https://code.google.com/p/jpcsp/source/detail?r=3082#

#include <cmath>
#include <limits>
#include <algorithm>

#include "math/math_util.h"

#include "Core/Core.h"
#include "Core/Reporting.h"
#include "Core/MemMap.h"

#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSInt.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MIPS/MIPSVFPUUtils.h"

#define R(i)   (currentMIPS->r[i])
#define V(i)   (currentMIPS->v[voffset[i]])
#define VI(i)  (currentMIPS->vi[voffset[i]])
#define FI(i)  (currentMIPS->fi[i])
#define FsI(i) (currentMIPS->fs[i])
#define PC     (currentMIPS->pc)

#define _RS   ((op>>21) & 0x1F)
#define _RT   ((op>>16) & 0x1F)
#define _RD   ((op>>11) & 0x1F)
#define _FS   ((op>>11) & 0x1F)
#define _FT   ((op>>16) & 0x1F)
#define _FD   ((op>>6 ) & 0x1F)
#define _POS  ((op>>6 ) & 0x1F)
#define _SIZE ((op>>11) & 0x1F)

#define HI currentMIPS->hi
#define LO currentMIPS->lo

#ifndef M_LOG2E
#define M_E        2.71828182845904523536f
#define M_LOG2E    1.44269504088896340736f
#define M_LOG10E   0.434294481903251827651f
#define M_LN2      0.693147180559945309417f
#define M_LN10     2.30258509299404568402f
#undef M_PI
#define M_PI       3.14159265358979323846f
#ifndef M_PI_2
#define M_PI_2     1.57079632679489661923f
#endif
#define M_PI_4     0.785398163397448309616f
#define M_1_PI     0.318309886183790671538f
#define M_2_PI     0.636619772367581343076f
#define M_2_SQRTPI 1.12837916709551257390f
#define M_SQRT2    1.41421356237309504880f
#define M_SQRT1_2  0.707106781186547524401f
#endif

union FloatBits {
	float f[4];
	u32 u[4];
	int i[4];
};

// Preserves NaN in first param, takes sign of equal second param.
// Technically, std::max may do this but it's undefined.
inline float nanmax(float f, float cst)
{
	return f <= cst ? cst : f;
}

// Preserves NaN in first param, takes sign of equal second param.
inline float nanmin(float f, float cst)
{
	return f >= cst ? cst : f;
}

// Preserves NaN in first param, takes sign of equal value in others.
inline float nanclamp(float f, float lower, float upper)
{
	return nanmin(nanmax(f, lower), upper);
}


void ApplyPrefixST(float *r, u32 data, VectorSize size)
{
	// Possible optimization shortcut:
	if (data == 0xe4)
		return;

	int n = GetNumVectorElements(size);
	float origV[4];
	static const float constantArray[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f/3.f, 0.25f, 1.f/6.f};

	for (int i = 0; i < n; i++)
	{
		origV[i] = r[i];
	}

	for (int i = 0; i < n; i++)
	{
		int regnum = (data >> (i*2)) & 3;
		int abs    = (data >> (8+i)) & 1;
		int negate = (data >> (16+i)) & 1;
		int constants = (data >> (12+i)) & 1;

		if (!constants)
		{
			// Prefix may say "z, z, z, z" but if this is a pair, we force to x.
			// TODO: But some ops seem to use const 0 instead?
			if (regnum >= n) {
				ERROR_LOG_REPORT(CPU, "Invalid VFPU swizzle: %08x: %i / %d at PC = %08x (%s)", data, regnum, n, currentMIPS->pc, MIPSDisasmAt(currentMIPS->pc));
				//for (int i = 0; i < 12; i++) {
				//	ERROR_LOG(CPU, "  vfpuCtrl[%i] = %08x", i, currentMIPS->vfpuCtrl[i]);
				//}
				regnum = 0;
			}

			r[i] = origV[regnum];
			if (abs)
				((u32 *)r)[i] = ((u32 *)r)[i] & 0x7FFFFFFF;
		}
		else
		{
			r[i] = constantArray[regnum + (abs<<2)];
		}

		if (negate)
			((u32 *)r)[i] = ((u32 *)r)[i] ^ 0x80000000;
	}
}

inline void ApplySwizzleS(float *v, VectorSize size)
{
	ApplyPrefixST(v, currentMIPS->vfpuCtrl[VFPU_CTRL_SPREFIX], size);
}

inline void ApplySwizzleT(float *v, VectorSize size)
{
	ApplyPrefixST(v, currentMIPS->vfpuCtrl[VFPU_CTRL_TPREFIX], size);
}

void ApplyPrefixD(float *v, VectorSize size, bool onlyWriteMask = false)
{
	u32 data = currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX];
	if (!data || onlyWriteMask)
		return;
	int n = GetNumVectorElements(size);
	for (int i = 0; i < n; i++)
	{
		int sat = (data >> (i * 2)) & 3;
		if (sat == 1)
			v[i] = vfpu_clamp(v[i], 0.0f, 1.0f);
		else if (sat == 3)
			v[i] = vfpu_clamp(v[i], -1.0f, 1.0f);
	}
}

static void RetainInvalidSwizzleST(float *d, VectorSize sz) {
	// Somehow it's like a supernan, maybe wires through to zero?
	// Doesn't apply to all ops.
	int sPrefix = currentMIPS->vfpuCtrl[VFPU_CTRL_SPREFIX];
	int tPrefix = currentMIPS->vfpuCtrl[VFPU_CTRL_TPREFIX];
	int n = GetNumVectorElements(sz);

	for (int i = 0; i < n; i++) {
		int swizzleS = (sPrefix >> (i + i)) & 3;
		int swizzleT = (tPrefix >> (i + i)) & 3;
		int constS = (sPrefix >> (12 + i)) & 1;
		int constT = (tPrefix >> (12 + i)) & 1;
		if ((swizzleS >= n && !constS) || (swizzleT >= n && !constT))
			d[i] = 0.0f;
	}
}

void EatPrefixes()
{
	currentMIPS->vfpuCtrl[VFPU_CTRL_SPREFIX] = 0xe4;  // passthru
	currentMIPS->vfpuCtrl[VFPU_CTRL_TPREFIX] = 0xe4;  // passthru
	currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX] = 0;
}

namespace MIPSInt
{
	void Int_VPFX(MIPSOpcode op)
	{
		int data = op & 0x000FFFFF;
		int regnum = (op >> 24) & 3;
		if (regnum == VFPU_CTRL_DPREFIX)
			data &= 0x00000FFF;
		currentMIPS->vfpuCtrl[VFPU_CTRL_SPREFIX + regnum] = data;
		PC += 4;
	}

	void Int_SVQ(MIPSOpcode op)
	{
		int imm = (signed short)(op&0xFFFC);
		int rs = _RS;
		int vt = (((op >> 16) & 0x1f)) | ((op&1) << 5);

		u32 addr = R(rs) + imm;

		switch (op >> 26)
		{
		case 53: //lvl.q/lvr.q
			{
				if (addr & 0x3)
				{
					_dbg_assert_msg_(CPU, 0, "Misaligned lvX.q");
				}
				float d[4];
				ReadVector(d, V_Quad, vt);
				int offset = (addr >> 2) & 3;
				if ((op & 2) == 0)
				{
					// It's an LVL
					for (int i = 0; i < offset + 1; i++)
					{
						d[3 - i] = Memory::Read_Float(addr - 4 * i);
					}
				}
				else
				{
					// It's an LVR
					for (int i = 0; i < (3 - offset) + 1; i++)
					{
						d[i] = Memory::Read_Float(addr + 4 * i);
					}
				}
				WriteVector(d, V_Quad, vt);
			}
			break;

		case 54: //lv.q
			if (addr & 0xF)
			{
				_dbg_assert_msg_(CPU, 0, "Misaligned lv.q");
			}
#ifndef COMMON_BIG_ENDIAN
			WriteVector((const float*)Memory::GetPointer(addr), V_Quad, vt);
#else
			float lvqd[4];

			lvqd[0] = Memory::Read_Float(addr);
			lvqd[1] = Memory::Read_Float(addr + 4);
			lvqd[2] = Memory::Read_Float(addr + 8);
			lvqd[3] = Memory::Read_Float(addr + 12);

			WriteVector(lvqd, V_Quad, vt);
#endif
			break;

		case 61: // svl.q/svr.q
			{
				if (addr & 0x3)
				{
					_dbg_assert_msg_(CPU, 0, "Misaligned svX.q");
				}
				float d[4];
				ReadVector(d, V_Quad, vt);
				int offset = (addr >> 2) & 3;
				if ((op&2) == 0)
				{
					// It's an SVL
					for (int i = 0; i < offset + 1; i++)
					{
						Memory::Write_Float(d[3 - i], addr - i * 4);
					}
				}
				else
				{
					// It's an SVR
					for (int i = 0; i < (3 - offset) + 1; i++)
					{
						Memory::Write_Float(d[i], addr + 4 * i);
					}
				}
				break;
			}

		case 62: //sv.q
			if (addr & 0xF)
			{
				_dbg_assert_msg_(CPU, 0, "Misaligned sv.q");
			}
#ifndef COMMON_BIG_ENDIAN
			ReadVector(reinterpret_cast<float *>(Memory::GetPointer(addr)), V_Quad, vt);
#else
			float svqd[4];
			ReadVector(svqd, V_Quad, vt);

			Memory::Write_Float(svqd[0], addr);
			Memory::Write_Float(svqd[1], addr + 4);
			Memory::Write_Float(svqd[2], addr + 8);
			Memory::Write_Float(svqd[3], addr + 12);
#endif
			break;

		default:
			_dbg_assert_msg_(CPU,0,"Trying to interpret VQ instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_VMatrixInit(MIPSOpcode op) {
		static const float idt[16] = {
			1,0,0,0,
			0,1,0,0,
			0,0,1,0,
			0,0,0,1,
		};
		static const float zero[16] = {
			0,0,0,0,
			0,0,0,0,
			0,0,0,0,
			0,0,0,0,
		};
		static const float one[16] = {
			1,1,1,1,
			1,1,1,1,
			1,1,1,1,
			1,1,1,1,
		};
		int vd = _VD;
		MatrixSize sz = GetMtxSize(op);
		const float *m;

		switch ((op >> 16) & 0xF) {
		case 3: m=idt; break; //identity   // vmidt
		case 6: m=zero; break;             // vmzero
		case 7: m=one; break;              // vmone
		default:
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
			PC += 4;
			EatPrefixes();
			return;
		}

		// The S prefix generates constants, but only for the final (possibly transposed) row.
		if (currentMIPS->vfpuCtrl[VFPU_CTRL_SPREFIX] & 0xF0F00) {
			float prefixed[16];
			memcpy(prefixed, m, sizeof(prefixed));

			int off = GetMatrixSide(sz) - 1;
			u32 sprefixRemove = VFPU_ANY_SWIZZLE();
			u32 sprefixAdd;
			switch ((op >> 16) & 0xF) {
			case 3:
			{
				VFPUConst constX = off == 0 ? VFPUConst::ONE : VFPUConst::ZERO;
				VFPUConst constY = off == 1 ? VFPUConst::ONE : VFPUConst::ZERO;
				VFPUConst constZ = off == 2 ? VFPUConst::ONE : VFPUConst::ZERO;
				VFPUConst constW = off == 3 ? VFPUConst::ONE : VFPUConst::ZERO;
				sprefixAdd = VFPU_MAKE_CONSTANTS(constX, constY, constZ, constW);
				break;
			}
			case 6:
				sprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::ZERO, VFPUConst::ZERO, VFPUConst::ZERO, VFPUConst::ZERO);
				break;
			case 7:
				sprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::ONE, VFPUConst::ONE, VFPUConst::ONE, VFPUConst::ONE);
				break;
			}
			ApplyPrefixST(&prefixed[off * 4], VFPURewritePrefix(VFPU_CTRL_SPREFIX, sprefixRemove, sprefixAdd), V_Quad);
			WriteMatrix(prefixed, sz, vd);
		} else {
			// Write mask applies to the final (maybe transposed) row.  Sat causes hang.
			WriteMatrix(m, sz, vd);
		}
		PC += 4;
		EatPrefixes();
	}

	void Int_VVectorInit(MIPSOpcode op)
	{
		int vd = _VD;
		VectorSize sz = GetVecSize(op);
		float d[4];

		VFPUConst constant = VFPUConst::ZERO;
		switch ((op >> 16) & 0xF) {
		case 6: constant = VFPUConst::ZERO; break;  //vzero
		case 7: constant = VFPUConst::ONE; break;   //vone
		default:
			_dbg_assert_msg_(CPU, 0, "Trying to interpret instruction that can't be interpreted");
			PC += 4;
			EatPrefixes();
			return;
		}

		// The S prefix generates constants, but negate is still respected.
		u32 sprefixRemove = VFPU_ANY_SWIZZLE();
		u32 sprefixAdd = VFPU_MAKE_CONSTANTS(constant, constant, constant, constant);
		ApplyPrefixST(d, VFPURewritePrefix(VFPU_CTRL_SPREFIX, sprefixRemove, sprefixAdd), sz);

		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);

		EatPrefixes();
		PC += 4;
	}

	void Int_Viim(MIPSOpcode op) {
		int vt = _VT;
		s32 imm = (s16)(op&0xFFFF);
		u16 uimm16 = (op&0xFFFF);
		float f[1];
		int type = (op >> 23) & 7;
		if (type == 6) {
			f[0] = (float)imm;  // viim
		} else if (type == 7) {
			f[0] = Float16ToFloat32((u16)uimm16);   // vfim
		} else {
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
			f[0] = 0;
		}
		
		ApplyPrefixD(f, V_Single);
		WriteVector(f, V_Single, vt);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vidt(MIPSOpcode op) {
		int vd = _VD;
		VectorSize sz = GetVecSize(op);
		float f[4];

		// The S prefix generates constants, but negate is still respected.
		int offmask = sz == V_Quad || sz == V_Triple ? 3 : 1;
		int off = vd & offmask;
		// If it's a pair, the identity starts in a different position.
		VFPUConst constX = off == (0 & offmask) ? VFPUConst::ONE : VFPUConst::ZERO;
		VFPUConst constY = off == (1 & offmask) ? VFPUConst::ONE : VFPUConst::ZERO;
		VFPUConst constZ = off == (2 & offmask) ? VFPUConst::ONE : VFPUConst::ZERO;
		VFPUConst constW = off == (3 & offmask) ? VFPUConst::ONE : VFPUConst::ZERO;

		u32 sprefixRemove = VFPU_ANY_SWIZZLE();
		u32 sprefixAdd = VFPU_MAKE_CONSTANTS(constX, constY, constZ, constW);
		ApplyPrefixST(f, VFPURewritePrefix(VFPU_CTRL_SPREFIX, sprefixRemove, sprefixAdd), sz);

		ApplyPrefixD(f, sz);
		WriteVector(f, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	// The test really needs some work.
	void Int_Vmmul(MIPSOpcode op) {
		float s[16]{}, t[16]{}, d[16];

		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		MatrixSize sz = GetMtxSize(op);
		int n = GetMatrixSide(sz);

		ReadMatrix(s, sz, vs);
		ReadMatrix(t, sz, vt);

		for (int a = 0; a < n; a++) {
			for (int b = 0; b < n; b++) {
				float sum = 0.0f;
				if (a == n - 1 && b == n - 1) {
					// S and T prefixes work on the final (or maybe first, in reverse?) dot.
					ApplySwizzleS(&s[b * 4], V_Quad);
					ApplySwizzleT(&t[a * 4], V_Quad);
					for (int c = 0; c < 4; c++) {
						sum += s[b * 4 + c] * t[a * 4 + c];
					}
				} else {
					for (int c = 0; c < n; c++) {
						sum += s[b * 4 + c] * t[a * 4 + c];
					}
				}
				d[a * 4 + b] = sum;
			}
		}

		// The D prefix applies ONLY to the final element, but sat does work.
		u32 lastmask = (currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX] & (1 << 8)) << (n - 1);
		u32 lastsat = (currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX] & 3) << (n + n - 2);
		currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX] = lastmask | lastsat;
		ApplyPrefixD(&d[4 * (n - 1)], V_Quad, false);
		WriteMatrix(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vmscl(MIPSOpcode op) {
		float s[16]{}, t[4]{}, d[16];

		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		MatrixSize sz = GetMtxSize(op);
		int n = GetMatrixSide(sz);

		ReadMatrix(s, sz, vs);
		ReadVector(t, V_Single, vt);

		for (int a = 0; a < n - 1; a++) {
			for (int b = 0; b < n; b++) {
				d[a * 4 + b] = s[a * 4 + b] * t[0];
			}
		}

		// S prefix applies to the last row.
		ApplySwizzleS(&s[(n - 1) * 4], V_Quad);
		// T prefix applies only for the last row, and is used per element.
		// This is like vscl, but instead of zzzz it uses xxxx.
		u32 tprefixRemove = VFPU_ANY_SWIZZLE();
		u32 tprefixAdd = VFPU_SWIZZLE(0, 0, 0, 0);
		ApplyPrefixST(t, VFPURewritePrefix(VFPU_CTRL_TPREFIX, tprefixRemove, tprefixAdd), V_Quad);

		for (int b = 0; b < n; b++) {
			d[(n - 1) * 4 + b] = s[(n - 1) * 4 + b] * t[b];
		}

		// The D prefix is applied to the last row.
		ApplyPrefixD(&d[(n - 1) * 4], V_Quad);
		WriteMatrix(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vmmov(MIPSOpcode op) {
		float s[16]{};
		int vd = _VD;
		int vs = _VS;
		MatrixSize sz = GetMtxSize(op);
		ReadMatrix(s, sz, vs);
		// S and D prefixes are applied to the last row.
		int off = GetMatrixSide(sz) - 1;
		ApplySwizzleS(&s[off * 4], V_Quad);
		ApplyPrefixD(&s[off * 4], V_Quad);
		WriteMatrix(s, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vflush(MIPSOpcode op)
	{
		VERBOSE_LOG(CPU, "vflush");
		PC += 4;
		// Anything with 0xFC000000 is a nop, but only 0xFFFF0000 retains prefixes.
		if ((op & 0xFFFF0000) != 0xFFFF0000)
			EatPrefixes();
	}

	void Int_VV2Op(MIPSOpcode op)
	{
		float s[4], d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		for (int i = 0; i < GetNumVectorElements(sz); i++)
		{
			switch ((op >> 16) & 0x1f)
			{
			case 0: d[i] = s[i]; break; //vmov
			case 1: d[i] = fabsf(s[i]); break; //vabs
			case 2: d[i] = -s[i]; break; //vneg
			// vsat0 changes -0.0 to +0.0, both retain NAN.
			case 4: if (s[i] <= 0) d[i] = 0; else {if(s[i] > 1.0f) d[i] = 1.0f; else d[i] = s[i];} break;    // vsat0
			case 5: if (s[i] < -1.0f) d[i] = -1.0f; else {if(s[i] > 1.0f) d[i] = 1.0f; else d[i] = s[i];} break;  // vsat1
			case 16: d[i] = 1.0f / s[i]; break; //vrcp
			case 17: d[i] = 1.0f / sqrtf(s[i]); break; //vrsq
				
			case 18: { d[i] = vfpu_sin(s[i]); } break; //vsin
			case 19: { d[i] = vfpu_cos(s[i]); } break; //vcos
			case 20: d[i] = powf(2.0f, s[i]); break; //vexp2
			case 21: d[i] = logf(s[i])/log(2.0f); break; //vlog2
			case 22: d[i] = fabsf(sqrtf(s[i])); break; //vsqrt
			case 23: d[i] = asinf(s[i]) / M_PI_2; break; //vasin
			case 24: d[i] = -1.0f / s[i]; break; // vnrcp
			case 26: { d[i] = -vfpu_sin(s[i]); } break; // vnsin
			case 28: d[i] = 1.0f / powf(2.0, s[i]); break; // vrexp2
			default:
				_dbg_assert_msg_(CPU,0,"Trying to interpret VV2Op instruction that can't be interpreted");
				break;
			}
		}
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vocp(MIPSOpcode op)
	{
		float s[4], t[4], d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);

		// S prefix forces the negate flags.
		u32 sprefixAdd = VFPU_NEGATE(1, 1, 1, 1);
		ApplyPrefixST(s, VFPURewritePrefix(VFPU_CTRL_SPREFIX, 0, sprefixAdd), sz);

		// T prefix forces constants on and regnum to 1.
		// That means negate still works, and abs activates a different constant.
		u32 tprefixRemove = VFPU_ANY_SWIZZLE();
		u32 tprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::ONE, VFPUConst::ONE, VFPUConst::ONE, VFPUConst::ONE);
		ApplyPrefixST(t, VFPURewritePrefix(VFPU_CTRL_TPREFIX, tprefixRemove, tprefixAdd), sz);

		for (int i = 0; i < GetNumVectorElements(sz); i++) {
			// Always positive NaN.  Note that s is always negated from the registers.
			d[i] = my_isnan(s[i]) ? fabsf(s[i]) : t[i] + s[i];
		}
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}
	
	void Int_Vsocp(MIPSOpcode op)
	{
		float s[4], t[4], d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		VectorSize outSize = GetDoubleVectorSize(sz);
		ReadVector(s, sz, vs);

		// S prefix forces negate in even/odd and xxyy swizzle.
		// abs works, and applies to final position (not source.)
		u32 sprefixRemove = VFPU_ANY_SWIZZLE() | VFPU_NEGATE(1, 1, 1, 1);
		u32 sprefixAdd = VFPU_SWIZZLE(0, 0, 1, 1) | VFPU_NEGATE(1, 0, 1, 0);
		ApplyPrefixST(s, VFPURewritePrefix(VFPU_CTRL_SPREFIX, sprefixRemove, sprefixAdd), outSize);

		// T prefix forces constants on and regnum to 1, 0, 1, 0.
		// That means negate still works, and abs activates a different constant.
		u32 tprefixRemove = VFPU_ANY_SWIZZLE();
		u32 tprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::ONE, VFPUConst::ZERO, VFPUConst::ONE, VFPUConst::ZERO);
		ApplyPrefixST(t, VFPURewritePrefix(VFPU_CTRL_TPREFIX, tprefixRemove, tprefixAdd), outSize);

		int n = GetNumVectorElements(sz);
		// Essentially D prefix saturation is forced.
		d[0] = nanclamp(t[0] + s[0], 0.0f, 1.0f);
		d[1] = nanclamp(t[1] + s[1], 0.0f, 1.0f);
		if (outSize == V_Quad) {
			d[2] = nanclamp(t[2] + s[2], 0.0f, 1.0f);
			d[3] = nanclamp(t[3] + s[3], 0.0f, 1.0f);
		}
		ApplyPrefixD(d, sz);
		WriteVector(d, outSize, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vsgn(MIPSOpcode op) {
		float s[4], t[4], d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);

		// Not sure who would do this, but using abs/neg allows a compare against 3 or -3.
		u32 tprefixRemove = VFPU_ANY_SWIZZLE();
		u32 tprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::ZERO, VFPUConst::ZERO, VFPUConst::ZERO, VFPUConst::ZERO);
		ApplyPrefixST(t, VFPURewritePrefix(VFPU_CTRL_TPREFIX, tprefixRemove, tprefixAdd), sz);

		int n = GetNumVectorElements(sz);
		if (n < 4) {
			// Compare with a swizzled value out of bounds always produces 0.
			memcpy(&s[n], &t[n], sizeof(float) * (4 - n));
		}
		ApplySwizzleS(s, V_Quad);

		for (int i = 0; i < n; i++) {
			float diff = s[i] - t[i];
			// To handle NaNs correctly, we do this with integer hackery
			u32 val;
			memcpy(&val, &diff, sizeof(u32));
			if (val == 0 || val == 0x80000000)
				d[i] = 0.0f;
			else if ((val >> 31) == 0)
				d[i] = 1.0f;
			else
				d[i] = -1.0f;
		}
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	inline int round_vfpu_n(double param) {
		// return floorf(param);
		return (int)round_ieee_754(param);
	}

	void Int_Vf2i(MIPSOpcode op) {
		float s[4];
		int d[4];
		int vd = _VD;
		int vs = _VS;
		int imm = (op >> 16) & 0x1f;
		float mult = (float)(1UL << imm);
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		// Negate, abs, and constants apply as you'd expect to the bits.
		ApplySwizzleS(s, sz);
		for (int i = 0; i < GetNumVectorElements(sz); i++) {
			if (my_isnan(s[i])) {
				d[i] = 0x7FFFFFFF;
				continue;
			}
			double sv = s[i] * mult; // (float)0x7fffffff == (float)0x80000000
			// Cap/floor it to 0x7fffffff / 0x80000000
			if (sv > (double)0x7fffffff) {
				d[i] = 0x7fffffff;
			} else if (sv <= (double)(int)0x80000000) {
				d[i] = 0x80000000;
			} else {
				switch ((op >> 21) & 0x1f)
				{
				case 16: d[i] = (int)round_vfpu_n(sv); break; //(floor(sv + 0.5f)); break; //n
				case 17: d[i] = s[i]>=0 ? (int)floor(sv) : (int)ceil(sv); break; //z
				case 18: d[i] = (int)ceil(sv); break; //u
				case 19: d[i] = (int)floor(sv); break; //d
				default: d[i] = 0x7FFFFFFF; break;
				}
			}
		}
		// Does not apply sat, but does apply mask.
		ApplyPrefixD(reinterpret_cast<float *>(d), sz, true);
		WriteVector(reinterpret_cast<float *>(d), sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vi2f(MIPSOpcode op) {
		int s[4];
		float d[4];
		int vd = _VD;
		int vs = _VS;
		int imm = (op >> 16) & 0x1f;
		float mult = 1.0f/(float)(1UL << imm);
		VectorSize sz = GetVecSize(op);
		ReadVector(reinterpret_cast<float *>(s), sz, vs);
		// Negate, abs, and constants apply as you'd expect to the bits.
		ApplySwizzleS(reinterpret_cast<float *>(s), sz);
		for (int i = 0; i < GetNumVectorElements(sz); i++) {
			d[i] = (float)s[i] * mult;
		}
		// Sat and mask apply normally.
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vh2f(MIPSOpcode op) {
		u32 s[4];
		float d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(reinterpret_cast<float *>(s), sz, vs);
		ApplySwizzleS(reinterpret_cast<float *>(s), sz);
		
		VectorSize outsize = V_Pair;
		switch (sz) {
		case V_Single:
			outsize = V_Pair;
			d[0] = ExpandHalf(s[0] & 0xFFFF);
			d[1] = ExpandHalf(s[0] >> 16);
			break;
		case V_Pair:
		default:
			// All other sizes are treated the same.
			outsize = V_Quad;
			d[0] = ExpandHalf(s[0] & 0xFFFF);
			d[1] = ExpandHalf(s[0] >> 16);
			d[2] = ExpandHalf(s[1] & 0xFFFF);
			d[3] = ExpandHalf(s[1] >> 16);
			break;
		}
		ApplyPrefixD(d, outsize);
		WriteVector(d, outsize, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vf2h(MIPSOpcode op) {
		float s[4]{};
		u32 d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		// Swizzle can cause V_Single to properly write both components.
		ApplySwizzleS(s, V_Quad);
		// Negate should not actually apply to invalid swizzle.
		RetainInvalidSwizzleST(s, V_Quad);
		
		VectorSize outsize = V_Single;
		switch (sz) {
		case V_Single:
		case V_Pair:
			outsize = V_Single;
			d[0] = ShrinkToHalf(s[0]) | ((u32)ShrinkToHalf(s[1]) << 16);
			break;
		case V_Triple:
		case V_Quad:
			outsize = V_Pair;
			d[0] = ShrinkToHalf(s[0]) | ((u32)ShrinkToHalf(s[1]) << 16);
			d[1] = ShrinkToHalf(s[2]) | ((u32)ShrinkToHalf(s[3]) << 16);
			break;
		}
		ApplyPrefixD(reinterpret_cast<float *>(d), outsize);
		WriteVector(reinterpret_cast<float *>(d), outsize, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vx2i(MIPSOpcode op)
	{
		u32 s[4];
		u32 d[4] = {0};
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		VectorSize oz = sz;
		ReadVector(reinterpret_cast<float *>(s), sz, vs);
		// ForbidVPFXS

		switch ((op >> 16) & 3) {
		case 0:  // vuc2i  
			// Quad is the only option.
			// This operation is weird. This particular way of working matches hw but does not 
			// seem quite sane.
			// I guess it's used for fixed-point math, and fills more bits to facilitate
			// conversion between 8-bit and 16-bit values.  But then why not do it in vc2i?
			{
				u32 value = s[0];
				for (int i = 0; i < 4; i++) {
					d[i] = (u32)((value & 0xFF) * 0x01010101) >> 1;
					value >>= 8;
				}
				oz = V_Quad;
			}
			break;

		case 1:  // vc2i
			// Quad is the only option
			{
				u32 value = s[0];
				d[0] = (value & 0xFF) << 24;
				d[1] = (value & 0xFF00) << 16;
				d[2] = (value & 0xFF0000) << 8;
				d[3] = (value & 0xFF000000);
				oz = V_Quad;
			}
			break;

		case 2:  // vus2i
			oz = V_Pair;
			switch (sz)
			{
			case V_Pair:
				oz = V_Quad;
				// Intentional fallthrough.
			case V_Single:
				for (int i = 0; i < GetNumVectorElements(sz); i++) {
					u32 value = s[i];
					d[i * 2] = (value & 0xFFFF) << 15;
					d[i * 2 + 1] = (value & 0xFFFF0000) >> 1;
				}
				break;

			default:
				ERROR_LOG_REPORT(CPU, "vus2i with more than 2 elements.");
				break;
			}
			break;

		case 3:  // vs2i
			oz = V_Pair;
			switch (sz)
			{
			case V_Pair:
				oz = V_Quad;
				// Intentional fallthrough.
			case V_Single:
				for (int i = 0; i < GetNumVectorElements(sz); i++) {
					u32 value = s[i];
					d[i * 2] = (value & 0xFFFF) << 16;
					d[i * 2 + 1] = value & 0xFFFF0000;
				}
				break;

			default:
				ERROR_LOG_REPORT(CPU, "vs2i with more than 2 elements.");
				break;
			}
			break;

		default:
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		
		ApplyPrefixD(reinterpret_cast<float *>(d),oz, true);  // Only write mask
		WriteVector(reinterpret_cast<float *>(d),oz,vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vi2x(MIPSOpcode op)
	{
		int s[4];
		u32 d[2] = {0};
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		VectorSize oz;
		ReadVector(reinterpret_cast<float *>(s), sz, vs);
		ApplySwizzleS(reinterpret_cast<float *>(s), sz); //TODO: and the mask to kill everything but swizzle
		switch ((op >> 16)&3)
		{
		case 0: //vi2uc
			{
				for (int i = 0; i < 4; i++)
				{
					int v = s[i];
					if (v < 0) v = 0;
					v >>= 23;
					d[0] |= ((u32)v & 0xFF) << (i * 8);
				}
				oz = V_Single;
			}
			break;

		case 1: //vi2c
			{
				for (int i = 0; i < 4; i++)
				{
					u32 v = s[i];
					d[0] |= (v >> 24) << (i * 8);
				}
				oz = V_Single;
			}
			break;

		case 2:  //vi2us
			{
				for (int i = 0; i < GetNumVectorElements(sz) / 2; i++) {
					int low = s[i * 2];
					int high = s[i * 2 + 1];
					if (low < 0) low = 0;
					if (high < 0) high = 0;
					low >>= 15;
					high >>= 15;
					d[i] = low | (high << 16);
				}
				switch (sz) {
				case V_Quad: oz = V_Pair; break;
				case V_Pair: oz = V_Single; break;
				default:
					_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
					oz = V_Single;
					break;
				}
			}
			break;
		case 3:  //vi2s
			{
				for (int i = 0; i < GetNumVectorElements(sz) / 2; i++) {
					u32 low = s[i * 2];
					u32 high = s[i * 2 + 1];
					low >>= 16;
					high >>= 16;
					d[i] = low | (high << 16);
				}
				switch (sz) {
				case V_Quad: oz = V_Pair; break;
				case V_Pair: oz = V_Single; break;
				default:
					_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
					oz = V_Single;
					break;
				}
			}
			break;
		default:
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
			oz = V_Single;
			break;
		}
		ApplyPrefixD(reinterpret_cast<float *>(d),oz);
		WriteVector(reinterpret_cast<float *>(d),oz,vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_ColorConv(MIPSOpcode op)
	{
		int vd = _VD;
		int vs = _VS;
		u32 s[4];
		VectorSize sz = V_Quad;
		ReadVector(reinterpret_cast<float *>(s), sz, vs);
		ApplySwizzleS(reinterpret_cast<float *>(s), sz);
		u16 colors[4];
		for (int i = 0; i < 4; i++)
		{
			u32 in = s[i];
			u16 col = 0;
			switch ((op >> 16) & 3)
			{
			case 1:  // 4444
				{
					int a = ((in >> 24) & 0xFF) >> 4;
					int b = ((in >> 16) & 0xFF) >> 4;
					int g = ((in >> 8) & 0xFF) >> 4;
					int r = ((in) & 0xFF) >> 4;
					col = (a << 12) | (b << 8) | (g << 4 ) | (r);
					break;
				}
			case 2:  // 5551
				{
					int a = ((in >> 24) & 0xFF) >> 7;
					int b = ((in >> 16) & 0xFF) >> 3;
					int g = ((in >> 8) & 0xFF) >> 3;
					int r = ((in) & 0xFF) >> 3;
					col = (a << 15) | (b << 10) | (g << 5) | (r);
					break;
				}
			case 3:  // 565
				{
					int b = ((in >> 16) & 0xFF) >> 3;
					int g = ((in >> 8) & 0xFF) >> 2;
					int r = ((in) & 0xFF) >> 3;
					col = (b << 11) | (g << 5) | (r); 
					break;
				}
			}
			colors[i] = col;
		}
		u32 ov[2] = {(u32)colors[0] | (colors[1] << 16), (u32)colors[2] | (colors[3] << 16)};
		ApplyPrefixD(reinterpret_cast<float *>(ov), V_Pair);
		WriteVector((const float *)ov, V_Pair, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_VDot(MIPSOpcode op) {
		float s[4]{}, t[4]{};
		float d;
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, V_Quad);
		ReadVector(t, sz, vt);
		ApplySwizzleT(t, V_Quad);
		d = 0.0f;
		for (int i = 0; i < 4; i++) {
			d += s[i] * t[i];
		}
		ApplyPrefixD(&d, V_Single);
		WriteVector(&d, V_Single, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_VHdp(MIPSOpcode op) {
		float s[4]{}, t[4]{};
		float d;
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ReadVector(t, sz, vt);
		ApplySwizzleT(t, V_Quad);

		// S prefix forces constant 1 for the last element (w for quad.)
		// Otherwise it is the same as vdot.
		u32 sprefixRemove;
		u32 sprefixAdd;
		if (sz == V_Quad) {
			sprefixRemove = VFPU_SWIZZLE(0, 0, 0, 3);
			sprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::NONE, VFPUConst::NONE, VFPUConst::NONE, VFPUConst::ONE);
		} else if (sz == V_Triple) {
			sprefixRemove = VFPU_SWIZZLE(0, 0, 3, 0);
			sprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::NONE, VFPUConst::NONE, VFPUConst::ONE, VFPUConst::NONE);
		} else if (sz == V_Pair) {
			sprefixRemove = VFPU_SWIZZLE(0, 3, 0, 0);
			sprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::NONE, VFPUConst::ONE, VFPUConst::NONE, VFPUConst::NONE);
		} else {
			sprefixRemove = VFPU_SWIZZLE(3, 0, 0, 0);
			sprefixAdd = VFPU_MAKE_CONSTANTS(VFPUConst::ONE, VFPUConst::NONE, VFPUConst::NONE, VFPUConst::NONE);
		}
		ApplyPrefixST(s, VFPURewritePrefix(VFPU_CTRL_SPREFIX, sprefixRemove, sprefixAdd), V_Quad);

		float sum = 0.0f;
		for (int i = 0; i < 4; i++) {
			sum += s[i] * t[i];
		}
		d = my_isnan(sum) ? fabsf(sum) : sum;
		ApplyPrefixD(&d, V_Single);
		WriteVector(&d, V_Single, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vbfy(MIPSOpcode op) {
		float s[4]{}, t[4]{}, d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ReadVector(t, sz, vs);

		int n = GetNumVectorElements(sz);
		if (op & 0x10000) {
			// vbfy2
			// S prefix forces the negate flags (so z and w are negative.)
			u32 sprefixAdd = VFPU_NEGATE(0, 0, 1, 1);
			ApplyPrefixST(s, VFPURewritePrefix(VFPU_CTRL_SPREFIX, 0, sprefixAdd), sz);

			// T prefix forces swizzle (zwxy.)
			// That means negate still works, but constants are a bit weird.
			u32 tprefixRemove = VFPU_ANY_SWIZZLE();
			u32 tprefixAdd = VFPU_SWIZZLE(2, 3, 0, 1);
			ApplyPrefixST(t, VFPURewritePrefix(VFPU_CTRL_TPREFIX, tprefixRemove, tprefixAdd), sz);

			// Other sizes don't seem completely predictable.
			if (sz != V_Quad) {
				ERROR_LOG_REPORT_ONCE(vbfy2, CPU, "vfby2 with incorrect size");
			}
		} else {
			// vbfy1
			// S prefix forces the negate flags (so y and w are negative.)
			u32 sprefixAdd = VFPU_NEGATE(0, 1, 0, 1);
			ApplyPrefixST(s, VFPURewritePrefix(VFPU_CTRL_SPREFIX, 0, sprefixAdd), sz);

			// T prefix forces swizzle (yxwz.)
			// That means negate still works, but constants are a bit weird.
			u32 tprefixRemove = VFPU_ANY_SWIZZLE();
			u32 tprefixAdd = VFPU_SWIZZLE(1, 0, 3, 2);
			ApplyPrefixST(t, VFPURewritePrefix(VFPU_CTRL_TPREFIX, tprefixRemove, tprefixAdd), sz);

			if (sz != V_Quad && sz != V_Pair) {
				ERROR_LOG_REPORT_ONCE(vbfy2, CPU, "vfby1 with incorrect size");
			}
		}

		d[0] = s[0] + t[0];
		d[1] = s[1] + t[1];
		d[2] = s[2] + t[2];
		d[3] = s[3] + t[3];

		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}
	
	void Int_Vsrt1(MIPSOpcode op)
	{
		float s[4];
		float d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		float x = s[0];
		float y = s[1];
		float z = s[2];
		float w = s[3];
		d[0] = std::min(x, y);
		d[1] = std::max(x, y);
		d[2] = std::min(z, w);
		d[3] = std::max(z, w);
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vsrt2(MIPSOpcode op)
	{
		float s[4];
		float d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		float x = s[0];
		float y = s[1];
		float z = s[2];
		float w = s[3];
		d[0] = std::min(x, w);
		d[1] = std::min(y, z);
		d[2] = std::max(y, z);
		d[3] = std::max(x, w);
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vsrt3(MIPSOpcode op)
	{
		float s[4];
		float d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		float x = s[0];
		float y = s[1];
		float z = s[2];
		float w = s[3];
		d[0] = std::max(x, y);
		d[1] = std::min(x, y);
		d[2] = std::max(z, w);
		d[3] = std::min(z, w);
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vsrt4(MIPSOpcode op)
	{
		float s[4];
		float d[4];
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		float x = s[0];
		float y = s[1];
		float z = s[2];
		float w = s[3];
		d[0] = std::max(x, w);
		d[1] = std::max(y, z);
		d[2] = std::min(y, z);
		d[3] = std::min(x, w);
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}
	
	void Int_Vcrs(MIPSOpcode op)
	{
		//half a cross product
		float s[4], t[4];
		float d[4];
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		VectorSize sz = GetVecSize(op);
		if (sz != V_Triple)
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");

		ReadVector(s, sz, vs);
		ReadVector(t, sz, vt);
		// no swizzles allowed
		d[0] = s[1] * t[2];
		d[1] = s[2] * t[0];
		d[2] = s[0] * t[1];
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}
	
	void Int_Vdet(MIPSOpcode op)
	{
		float s[4], t[4];
		float d[4];
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		VectorSize sz = GetVecSize(op);
		if (sz != V_Pair)
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		ReadVector(t, sz, vt);
		// TODO: The swizzle on t behaves oddly with constants, but sign changes seem to work.
		// Also, seems to round in a non-standard way (sometimes toward zero, not always.)
		d[0] = s[0] * t[1] - s[1] * t[0];
		ApplyPrefixD(d, sz);
		WriteVector(d, V_Single, vd);
		PC += 4;
		EatPrefixes();
	}
	
	void Int_Vfad(MIPSOpcode op)
	{
		float s[4];
		float d;
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		float sum = 0.0f;
		int n = GetNumVectorElements(sz);
		for (int i = 0; i < n; i++)
		{
			sum += s[i];
		}
		d = sum;
		ApplyPrefixD(&d,V_Single);
		V(vd) = d;
		PC += 4;
		EatPrefixes();
	}

	void Int_Vavg(MIPSOpcode op)
	{
		float s[4];
		float d;
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		float sum = 0.0f;
		int n = GetNumVectorElements(sz);
		for (int i = 0; i < n; i++)
		{
			sum += s[i];
		}
		d = sum / n;
		ApplyPrefixD(&d, V_Single);
		V(vd) = d;
		PC += 4;
		EatPrefixes();
	}

	void Int_VScl(MIPSOpcode op) {
		float s[4], t[4], d[4];
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		VectorSize sz = GetVecSize(op);
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);

		// T prefix forces swizzle (zzzz for some reason, so we force V_Quad.)
		// That means negate still works, but constants are a bit weird.
		t[2] = V(vt);
		u32 tprefixRemove = VFPU_ANY_SWIZZLE();
		u32 tprefixAdd = VFPU_SWIZZLE(2, 2, 2, 2);
		ApplyPrefixST(t, VFPURewritePrefix(VFPU_CTRL_TPREFIX, tprefixRemove, tprefixAdd), V_Quad);

		int n = GetNumVectorElements(sz);
		for (int i = 0; i < n; i++) {
			d[i] = s[i] * t[i];
		}
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vrnds(MIPSOpcode op) {
		int vd = _VD;
		int seed = VI(vd);
		// Swizzles apply a constant value, constants/abs/neg work to vary the seed.
		ApplySwizzleS(reinterpret_cast<float *>(&seed), V_Single);
		currentMIPS->rng.Init(seed);
		PC += 4;
		EatPrefixes();
	}

	void Int_VrndX(MIPSOpcode op) {
		FloatBits d;
		int vd = _VD;
		VectorSize sz = GetVecSize(op);
		int n = GetNumVectorElements(sz);
		for (int i = 0; i < n; i++) {
			switch ((op >> 16) & 0x1f) {
			case 1: d.u[i] = currentMIPS->rng.R32(); break;  // vrndi
			case 2: d.f[i] = 1.0f + ((float)currentMIPS->rng.R32() / 0xFFFFFFFF); break; // vrndf1   TODO: make more accurate
			case 3: d.f[i] = 2.0f + 2 * ((float)currentMIPS->rng.R32() / 0xFFFFFFFF); break; // vrndf2   TODO: make more accurate
			default: _dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
			}
		}
		// D prefix is broken and applies to the last element only (mask and sat.)
		u32 lastmask = (currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX] & (1 << 8)) << (n - 1);
		u32 lastsat = (currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX] & 3) << (n + n - 2);
		currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX] = lastmask | lastsat;
		ApplyPrefixD(d.f, sz);
		WriteVector(d.f, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	// Generates one line of a rotation matrix around one of the three axes
	void Int_Vrot(MIPSOpcode op) {
		float d[4]{};
		int vd = _VD;
		int vs = _VS;
		int imm = (op >> 16) & 0x1f;
		VectorSize sz = GetVecSize(op);
		bool negSin = (imm & 0x10) != 0;
		int sineLane = (imm >> 2) & 3;
		int cosineLane = imm & 3;

		float sine, cosine;
		if (currentMIPS->vfpuCtrl[VFPU_CTRL_SPREFIX] == 0x000E4) {
			vfpu_sincos(V(vs), sine, cosine);
			if (negSin)
				sine = -sine;
		} else {
			// Swizzle on S is a bit odd here, but generally only applies to sine.
			float s[4]{};
			ReadVector(s, V_Single, vs);
			u32 sprefixRemove = VFPU_NEGATE(1, 0, 0, 0);
			// We apply negSin later, not here.  This handles zero a bit better.
			u32 sprefixAdd = VFPU_NEGATE(0, 0, 0, 0);
			ApplyPrefixST(s, VFPURewritePrefix(VFPU_CTRL_SPREFIX, sprefixRemove, sprefixAdd), V_Single);

			// Cosine ignores all prefixes, so take the original.
			cosine = vfpu_cos(V(vs));
			sine = vfpu_sin(s[0]);

			if (negSin)
				sine = -sine;
			RetainInvalidSwizzleST(&sine, V_Single);
		}

		if (sineLane == cosineLane) {
			for (int i = 0; i < 4; i++)
				d[i] = sine;
		} else {
			d[sineLane] = sine;
		}
		d[cosineLane] = cosine;

		// D prefix works, just not for x.
		currentMIPS->vfpuCtrl[VFPU_CTRL_DPREFIX] &= 0xFFEFC;
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vtfm(MIPSOpcode op)
	{
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		int ins = (op >> 23) & 7;

		VectorSize sz = GetVecSize(op);
		MatrixSize msz = GetMtxSize(op);
		int n = GetNumVectorElements(sz);

		bool homogenous = false;
		if (n == ins)
		{
			n++;
			sz = (VectorSize)((int)(sz) + 1);
			msz = (MatrixSize)((int)(msz) + 1);
			homogenous = true;
		}

		float s[16];
		ReadMatrix(s, msz, vs);
		float t[4];
		ReadVector(t, sz, vt);
		float d[4];

		if (homogenous)
		{
			for (int i = 0; i < n; i++)
			{
				d[i] = 0.0f;
				for (int k = 0; k < n; k++)
				{
					d[i] += (k == n-1) ? s[i*4+k] : (s[i*4+k] * t[k]);
				}
			}
		}
		else if (n == ins + 1)
		{
			for (int i = 0; i < n; i++)
			{
				d[i] = s[i*4] * t[0];
				for (int k = 1; k < n; k++)
				{
					d[i] += s[i*4+k] * t[k];
				}
			}
		}
		else
		{
			Reporting::ReportMessage("Trying to interpret instruction that can't be interpreted (BADVTFM)");
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted (BADVTFM)");
			for (int i = 0; i < n; i++)
				d[i] = 0.0;
		}
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}
 
	void Int_SV(MIPSOpcode op)
	{
		s32 imm = (signed short)(op&0xFFFC);
		int vt = ((op >> 16) & 0x1f) | ((op & 3) << 5);
		int rs = _RS;
		u32 addr = R(rs) + imm;

		switch (op >> 26)
		{
		case 50: //lv.s
			VI(vt) = Memory::Read_U32(addr);
			break;
		case 58: //sv.s
			Memory::Write_U32(VI(vt), addr);
			break;
		default:
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}


	void Int_Mftv(MIPSOpcode op)
	{
		int imm = op & 0xFF;
		int rt = _RT;
		switch ((op >> 21) & 0x1f)
		{
		case 3: //mfv / mfvc
			// rt = 0, imm = 255 appears to be used as a CPU interlock by some games.
			if (rt != 0) {
				if (imm < 128) {
					R(rt) = VI(imm);
				} else if (imm < 128 + VFPU_CTRL_MAX) { //mfvc
					R(rt) = currentMIPS->vfpuCtrl[imm - 128];
				} else {
					//ERROR - maybe need to make this value too an "interlock" value?
					_dbg_assert_msg_(CPU,0,"mfv - invalid register");
				}
			}
			break;

		case 7: //mtv
			if (imm < 128) {
				VI(imm) = R(rt);
			} else if (imm < 128 + VFPU_CTRL_MAX) { //mtvc
				u32 mask;
				if (GetVFPUCtrlMask(imm - 128, &mask)) {
					currentMIPS->vfpuCtrl[imm - 128] = R(rt) & mask;
				}
			} else {
				//ERROR
				_dbg_assert_msg_(CPU,0,"mtv - invalid register");
			}
			break;

		default:
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_Vmfvc(MIPSOpcode op) {
		int vd = _VD;
		int imm = (op >> 8) & 0x7F;
		if (imm < VFPU_CTRL_MAX) {
			VI(vd) = currentMIPS->vfpuCtrl[imm];
		} else {
			VI(vd) = 0;
		}
		PC += 4;
	}

	void Int_Vmtvc(MIPSOpcode op) {
		int vs = _VS;
		int imm = op & 0x7F;
		if (imm < VFPU_CTRL_MAX) {
			u32 mask;
			if (GetVFPUCtrlMask(imm, &mask)) {
				currentMIPS->vfpuCtrl[imm] = VI(vs) & mask;
			}
		}
		PC += 4;
	}

	void Int_Vcst(MIPSOpcode op)
	{
		int conNum = (op >> 16) & 0x1f;
		int vd = _VD;

		VectorSize sz = GetVecSize(op);
		float c = cst_constants[conNum];
		float temp[4] = {c,c,c,c};
		ApplyPrefixD(temp, sz);
		WriteVector(temp, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vcmp(MIPSOpcode op)
	{
		int vs = _VS;
		int vt = _VT;
		int cond = op & 0xf;
		VectorSize sz = GetVecSize(op);
		int n = GetNumVectorElements(sz);
		float s[4];
		float t[4];
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		ReadVector(t, sz, vt);
		ApplySwizzleT(t, sz);
		int cc = 0;
		int or_val = 0;
		int and_val = 1;
		int affected_bits = (1 << 4) | (1 << 5);  // 4 and 5
		for (int i = 0; i < n; i++)
		{
			int c;
			// These set c to 0 or 1, nothing else.
			switch (cond)
			{
			case VC_FL: c = 0; break;
			case VC_EQ: c = s[i] == t[i]; break;
			case VC_LT: c = s[i] < t[i]; break;
			case VC_LE: c = s[i] <= t[i]; break;

			case VC_TR: c = 1; break;
			case VC_NE: c = s[i] != t[i]; break;
			case VC_GE: c = s[i] >= t[i]; break;
			case VC_GT: c = s[i] > t[i]; break;

			case VC_EZ: c = s[i] == 0.0f || s[i] == -0.0f; break;
			case VC_EN: c = my_isnan(s[i]); break;
			case VC_EI: c = my_isinf(s[i]); break;
			case VC_ES: c = my_isnanorinf(s[i]); break;   // Tekken Dark Resurrection

			case VC_NZ: c = s[i] != 0; break;
			case VC_NN: c = !my_isnan(s[i]); break;
			case VC_NI: c = !my_isinf(s[i]); break;
			case VC_NS: c = !(my_isnanorinf(s[i])); break;   // How about t[i] ?

			default:
				_dbg_assert_msg_(CPU,0,"Unsupported vcmp condition code %d", cond);
				PC += 4;
				EatPrefixes();
				return;
			}
			cc |= (c<<i);
			or_val |= c;
			and_val &= c;
			affected_bits |= 1 << i;
		}
		// Use masking to only change the affected bits
		currentMIPS->vfpuCtrl[VFPU_CTRL_CC] =
			(currentMIPS->vfpuCtrl[VFPU_CTRL_CC] & ~affected_bits) |
			((cc | (or_val << 4) | (and_val << 5)) & affected_bits);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vminmax(MIPSOpcode op) {
		int vt = _VT;
		int vs = _VS;
		int vd = _VD;
		int cond = op&15;
		VectorSize sz = GetVecSize(op);
		int numElements = GetNumVectorElements(sz);

		FloatBits s;
		FloatBits t;
		FloatBits d;
		ReadVector(s.f, sz, vs);
		ApplySwizzleS(s.f, sz);
		ReadVector(t.f, sz, vt);
		ApplySwizzleT(t.f, sz);

		// If both are zero, take t's sign.
		// Otherwise: -NAN < -INF < real < INF < NAN (higher mantissa is farther from 0.)

		switch ((op >> 23) & 3) {
		case 2: // vmin
			for (int i = 0; i < numElements; i++) {
				if (my_isnanorinf(s.f[i]) || my_isnanorinf(t.f[i])) {
					// If both are negative, we flip the comparison (not two's compliment.)
					if (s.i[i] < 0 && t.i[i] < 0) {
						// If at least one side is NAN, we take the highest mantissa bits.
						d.i[i] = std::max(t.i[i], s.i[i]);
					} else {
						// Otherwise, we take the lowest value (negative or lowest mantissa.)
						d.i[i] = std::min(t.i[i], s.i[i]);
					}
				} else {
					d.f[i] = std::min(t.f[i], s.f[i]);
				}
			}
			break;
		case 3: // vmax
			for (int i = 0; i < numElements; i++) {
				// This is the same logic as vmin, just reversed.
				if (my_isnanorinf(s.f[i]) || my_isnanorinf(t.f[i])) {
					if (s.i[i] < 0 && t.i[i] < 0) {
						d.i[i] = std::min(t.i[i], s.i[i]);
					} else {
						d.i[i] = std::max(t.i[i], s.i[i]);
					}
				} else {
					d.f[i] = std::max(t.f[i], s.f[i]);
				}
			}
			break;
		default:
			_dbg_assert_msg_(CPU,0,"unknown min/max op %d", cond);
			PC += 4;
			EatPrefixes();
			return;
		}
		ApplyPrefixD(d.f, sz);
		WriteVector(d.f, sz, vd);
		PC += 4;
		EatPrefixes();
	}
	
	void Int_Vscmp(MIPSOpcode op) {
		FloatBits s, t, d;
		int vt = _VT;
		int vs = _VS;
		int vd = _VD;
		VectorSize sz = GetVecSize(op);
		ReadVector(s.f, sz, vs);
		ApplySwizzleS(s.f, sz);
		ReadVector(t.f, sz, vt);
		ApplySwizzleT(t.f, sz);
		int n = GetNumVectorElements(sz);
		for (int i = 0; i < n ; i++) {
			float a = s.f[i] - t.f[i];
			if (my_isnan(a)) {
				// NAN/INF are treated as just larger numbers, as in vmin/vmax.
				int sMagnitude = s.u[i] & 0x7FFFFFFF;
				int tMagnitude = t.u[i] & 0x7FFFFFFF;
				int b = (s.i[i] < 0 ? -sMagnitude : sMagnitude) - (t.i[i] < 0 ? -tMagnitude : tMagnitude);
				d.f[i] = (float)((0 < b) - (b < 0));
			} else {
				d.f[i] = (float)((0.0f < a) - (a < 0.0f));
			}
		}
		RetainInvalidSwizzleST(d.f, sz);
		ApplyPrefixD(d.f, sz);
		WriteVector(d.f, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vsge(MIPSOpcode op) {
		int vt = _VT;
		int vs = _VS;
		int vd = _VD;
		VectorSize sz = GetVecSize(op);
		int numElements = GetNumVectorElements(sz);
		float s[4];
		float t[4];
		float d[4];
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		ReadVector(t, sz, vt);
		ApplySwizzleT(t, sz);
		for (int i = 0; i < GetNumVectorElements(sz); i++) {
			if ( my_isnan(s[i]) || my_isnan(t[i]) )
				d[i] = 0.0f;
			else
				d[i] = s[i] >= t[i] ? 1.0f : 0.0f;
		}
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vslt(MIPSOpcode op) {
		int vt = _VT;
		int vs = _VS;
		int vd = _VD;
		VectorSize sz = GetVecSize(op);
		int numElements = GetNumVectorElements(sz);
		float s[4];
		float t[4];
		float d[4];
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		ReadVector(t, sz, vt);
		ApplySwizzleT(t, sz);
		for (int i = 0; i < GetNumVectorElements(sz); i++) {
			if ( my_isnan(s[i]) || my_isnan(t[i]) )
				d[i] = 0.0f;
			else
				d[i] = s[i] < t[i] ? 1.0f : 0.0f;
		}
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}


	void Int_Vcmov(MIPSOpcode op) {
		int vs = _VS;
		int vd = _VD;
		int tf = (op >> 19) & 1;
		int imm3 = (op >> 16) & 7;
		VectorSize sz = GetVecSize(op);
		int n = GetNumVectorElements(sz);
		float s[4];
		float d[4];
		ReadVector(s, sz, vs);
		ApplySwizzleS(s, sz);
		// Not only is D read (as T), but the T prefix applies to it.
		ReadVector(d, sz, vd);
		ApplySwizzleT(d, sz);

		int CC = currentMIPS->vfpuCtrl[VFPU_CTRL_CC];

		if (imm3 < 6) {
			if (((CC >> imm3) & 1) == !tf) {
				for (int i = 0; i < n; i++)
					d[i] = s[i];
			}
		} else if (imm3 == 6) {
			for (int i = 0; i < n; i++) {
				if (((CC >> i) & 1) == !tf)
					d[i] = s[i];
			}
		} else {
			ERROR_LOG_REPORT(CPU, "Bad Imm3 in cmov: %d", imm3);
		}
		ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_VecDo3(MIPSOpcode op) {
		float s[4], t[4], d[4];
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		VectorSize sz = GetVecSize(op);

		int optype = 0;
		switch (op >> 26) {
		case 24: //VFPU0
			switch ((op >> 23) & 7) {
			case 0: optype = 0; break;
			case 1: optype = 1; break;
			case 7: optype = 7; break;
			default: goto bad;
			}
			break;
		case 25: //VFPU1
			switch ((op >> 23) & 7) {
			case 0: optype = 8; break;
			default: goto bad;
			}
			break;
		default:
		bad:
			_dbg_assert_msg_(CPU, 0, "Trying to interpret instruction that can't be interpreted");
			break;
		}

		int n = GetNumVectorElements(sz);
		ReadVector(s, sz, vs);
		ReadVector(t, sz, vt);
		if (optype != 7) {
			ApplySwizzleS(s, sz);
			ApplySwizzleT(t, sz);
		} else {
			// The prefix handling of S/T is a bit odd, probably the HW doesn't do it in parallel.
			// The X prefix is applied to the last element in sz.
			ApplySwizzleS(&s[n - 1], V_Single);
			ApplySwizzleT(&t[n - 1], V_Single);
		}

		for (int i = 0; i < n; i++) {
			switch (optype) {
			case 0: d[i] = s[i] + t[i]; break; //vadd
			case 1: d[i] = s[i] - t[i]; break; //vsub
			case 7: d[i] = s[i] / t[i]; break; //vdiv
			case 8: d[i] = s[i] * t[i]; break; //vmul
			}
		}

		// The D prefix (even mask) is ignored by vdiv only (vmul, etc. do apply it.)
		if (optype != 7)
			ApplyPrefixD(d, sz);
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}
	
	void Int_CrossQuat(MIPSOpcode op)
	{
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		VectorSize sz = GetVecSize(op);
		float s[4];
		float t[4];
		float d[4];
		ReadVector(s, sz, vs);
		ReadVector(t, sz, vt);
		switch (sz)
		{
		case V_Triple:  // vcrsp.t
			d[0] = s[1]*t[2] - s[2]*t[1];
			d[1] = s[2]*t[0] - s[0]*t[2];
			d[2] = s[0]*t[1] - s[1]*t[0];
			break;

		case V_Quad:   // vqmul.q
			d[0] = s[0]*t[3] + s[1]*t[2] - s[2]*t[1] + s[3]*t[0];
			d[1] = -s[0]*t[2] + s[1]*t[3] + s[2]*t[0] + s[3]*t[1];
			d[2] = s[0]*t[1] - s[1]*t[0] + s[2]*t[3] + s[3]*t[2];
			d[3] = -s[0]*t[0] - s[1]*t[1] - s[2]*t[2] + s[3]*t[3];
			break;

		default:
			Reporting::ReportMessage("CrossQuat instruction with wrong size");
			_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
			d[0] = 0;
			d[1] = 0;
			break;
		}
		WriteVector(d, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vlgb(MIPSOpcode op)
	{
		// Vector log binary (extract exponent)
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);

		FloatBits d;
		FloatBits s;

		ReadVector(s.f, sz, vs);
		// TODO: Test swizzle, t?
		ApplySwizzleS(s.f, sz);

		if (sz != V_Single) {
			ERROR_LOG_REPORT(CPU, "vlgb not implemented for size %d", GetNumVectorElements(sz));
		}
		for (int i = 0; i < GetNumVectorElements(sz); ++i) {
			int exp = (s.u[i] & 0x7F800000) >> 23;
			if (exp == 0xFF) {
				d.f[i] = s.f[i];
			} else if (exp == 0) {
				d.f[i] = -INFINITY;
			} else {
				d.f[i] = (float)(exp - 127);
			}
		}
		ApplyPrefixD(d.f, sz);
		WriteVector(d.f, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	// There has to be a concise way of expressing this in terms of
	// bit manipulation on the raw floats.
	void Int_Vwbn(MIPSOpcode op) {
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);

		FloatBits d;
		FloatBits s;
		u8 exp = (u8)((op >> 16) & 0xFF);

		ReadVector(s.f, sz, vs);
		// TODO: Test swizzle, t?
		ApplySwizzleS(s.f, sz);

		if (sz != V_Single) {
			ERROR_LOG_REPORT(CPU, "vwbn not implemented for size %d", GetNumVectorElements(sz));
		}
		for (int i = 0; i < GetNumVectorElements(sz); ++i) {
			u32 sigbit = s.u[i] & 0x80000000;
			u32 prevExp = (s.u[i] & 0x7F800000) >> 23;
			u32 mantissa = (s.u[i] & 0x007FFFFF) | 0x00800000;
			if (prevExp != 0xFF && prevExp != 0) {
				if (exp > prevExp) {
					s8 shift = (exp - prevExp) & 0xF;
					mantissa = mantissa >> shift;
				} else {
					s8 shift = (prevExp - exp) & 0xF;
					mantissa = mantissa << shift;
				}
				d.u[i] = sigbit | (mantissa & 0x007FFFFF) | (exp << 23);
			} else {
				d.u[i] = s.u[i] | (exp << 23);
			}
		}
		ApplyPrefixD(d.f, sz);
		WriteVector(d.f, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vsbn(MIPSOpcode op)
	{
		int vd = _VD;
		int vs = _VS;
		int vt = _VT;
		VectorSize sz = GetVecSize(op);

		FloatBits d;
		FloatBits s;
		u8 exp = (u8)(127 + VI(vt));

		ReadVector(s.f, sz, vs);
		// TODO: Test swizzle, t?
		ApplySwizzleS(s.f, sz);

		if (sz != V_Single) {
			ERROR_LOG_REPORT(CPU, "vsbn not implemented for size %d", GetNumVectorElements(sz));
		}
		for (int i = 0; i < GetNumVectorElements(sz); ++i) {
			// Simply replace the exponent bits.
			u32 prev = s.u[i] & 0x7F800000;
			if (prev != 0 && prev != 0x7F800000) {
				d.u[i] = (s.u[i] & ~0x7F800000) | (exp << 23);
			} else {
				d.u[i] = s.u[i];
			}
		}
		ApplyPrefixD(d.f, sz);
		WriteVector(d.f, sz, vd);
		PC += 4;
		EatPrefixes();
	}

	void Int_Vsbz(MIPSOpcode op)
	{
		// Vector scale by zero (set exp to 0 to extract mantissa)
		int vd = _VD;
		int vs = _VS;
		VectorSize sz = GetVecSize(op);

		FloatBits d;
		FloatBits s;

		ReadVector(s.f, sz, vs);
		// TODO: Test swizzle, t?
		ApplySwizzleS(s.f, sz);

		if (sz != V_Single) {
			ERROR_LOG_REPORT(CPU, "vsbz not implemented for size %d", GetNumVectorElements(sz));
		}
		for (int i = 0; i < GetNumVectorElements(sz); ++i) {
			// NAN and denormals pass through.
			if (my_isnan(s.f[i]) || (s.u[i] & 0x7F800000) == 0) {
				d.u[i] = s.u[i];
			} else {
				d.u[i] = (127 << 23) | (s.u[i] & 0x007FFFFF);
			}
		}
		ApplyPrefixD(d.f, sz);
		WriteVector(d.f, sz, vd);
		PC += 4;
		EatPrefixes();
	}
}
