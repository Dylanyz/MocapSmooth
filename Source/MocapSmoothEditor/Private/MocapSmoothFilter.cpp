// SPDX-License-Identifier: Apache-2.0

#include "MocapSmoothFilter.h"

#include <complex>

DEFINE_LOG_CATEGORY_STATIC(LogMocapSmooth, Log, All);

namespace
{
	using FComplex = std::complex<double>;

	/** Coefficients of prod(x - Roots[i]), highest power first. Imaginary parts cancel for
	    conjugate-paired roots, which is what a Butterworth prototype always gives us. */
	void PolyFromRoots(const TArray<FComplex>& Roots, TArray<double>& OutCoeffs)
	{
		TArray<FComplex> P;
		P.Add(FComplex(1.0, 0.0));
		for (const FComplex& R : Roots)
		{
			TArray<FComplex> Next;
			Next.SetNumZeroed(P.Num() + 1);
			for (int32 i = 0; i < P.Num(); ++i)
			{
				Next[i] += P[i];
				Next[i + 1] -= P[i] * R;
			}
			P = MoveTemp(Next);
		}
		OutCoeffs.SetNumUninitialized(P.Num());
		for (int32 i = 0; i < P.Num(); ++i)
		{
			OutCoeffs[i] = P[i].real();
		}
	}

	/** Steady-state initial conditions for a step input, so the filter starts settled.
	    Solves (I - A) z = B for the Direct Form 2 Transposed state. */
	void LFilterZi(const TArray<double>& B, const TArray<double>& A, TArray<double>& OutZi)
	{
		const int32 N = A.Num();          // == Order + 1
		const int32 M = N - 1;
		OutZi.SetNumZeroed(FMath::Max(M, 0));
		if (M <= 0)
		{
			return;
		}

		// Companion-form matrix A as used by the reference: first column -a[1:], super-diagonal I.
		TArray<double> Mat;               // (I - A), row-major M x M, augmented with B
		Mat.SetNumZeroed(M * (M + 1));
		auto At = [&Mat, M](int32 r, int32 c) -> double& { return Mat[r * (M + 1) + c]; };

		for (int32 r = 0; r < M; ++r)
		{
			At(r, 0) += A[r + 1];         // I - A, and A's first column is -a[1:]
			At(r, r) += 1.0;
			if (r < M - 1)
			{
				At(r, r + 1) -= 1.0;      // minus the super-diagonal identity
			}
			At(r, M) = B[r + 1] - A[r + 1] * B[0];
		}

		// Gaussian elimination with partial pivoting. M is at most 4 here.
		for (int32 col = 0; col < M; ++col)
		{
			int32 Pivot = col;
			for (int32 r = col + 1; r < M; ++r)
			{
				if (FMath::Abs(At(r, col)) > FMath::Abs(At(Pivot, col)))
				{
					Pivot = r;
				}
			}
			if (FMath::IsNearlyZero(At(Pivot, col), UE_DOUBLE_SMALL_NUMBER))
			{
				continue;
			}
			if (Pivot != col)
			{
				for (int32 c = 0; c <= M; ++c)
				{
					Swap(At(col, c), At(Pivot, c));
				}
			}
			const double Diag = At(col, col);
			for (int32 c = col; c <= M; ++c)
			{
				At(col, c) /= Diag;
			}
			for (int32 r = 0; r < M; ++r)
			{
				if (r == col)
				{
					continue;
				}
				const double Factor = At(r, col);
				if (Factor == 0.0)
				{
					continue;
				}
				for (int32 c = col; c <= M; ++c)
				{
					At(r, c) -= Factor * At(col, c);
				}
			}
		}
		for (int32 r = 0; r < M; ++r)
		{
			OutZi[r] = At(r, M);
		}
	}

	/** Direct Form 2 Transposed IIR over interleaved channels, in place. */
	void LFilter(const TArray<double>& B, const TArray<double>& A, const TArray<double>& Zi,
	             TArray<double>& X, int32 NumChannels, int32 NumSamples, bool bReverse)
	{
		const int32 N = B.Num();
		const int32 M = N - 1;
		if (M <= 0 || NumSamples <= 0)
		{
			return;
		}

		TArray<double> D;
		D.SetNumZeroed(M * NumChannels);

		// Initial state is zi scaled by the first sample of each channel, so a constant run in
		// stays a constant run out instead of ramping up from zero.
		const int32 FirstIdx = bReverse ? (NumSamples - 1) : 0;
		for (int32 c = 0; c < NumChannels; ++c)
		{
			const double First = X[FirstIdx * NumChannels + c];
			for (int32 j = 0; j < M; ++j)
			{
				D[j * NumChannels + c] = Zi[j] * First;
			}
		}

		for (int32 s = 0; s < NumSamples; ++s)
		{
			const int32 Idx = (bReverse ? (NumSamples - 1 - s) : s) * NumChannels;
			for (int32 c = 0; c < NumChannels; ++c)
			{
				const double Xi = X[Idx + c];
				const double Yi = B[0] * Xi + D[c];
				for (int32 j = 0; j < M - 1; ++j)
				{
					D[j * NumChannels + c] =
						B[j + 1] * Xi + D[(j + 1) * NumChannels + c] - A[j + 1] * Yi;
				}
				D[(M - 1) * NumChannels + c] = B[M] * Xi - A[M] * Yi;
				X[Idx + c] = Yi;
			}
		}
	}
}

namespace MocapSmoothFilter
{

double SliderToCutoffHz(double Slider)
{
	return 10.5 - FMath::Clamp(Slider, 0.5, 10.0);
}

bool DesignButterworth(int32 Order, double CutoffHz, double SampleRateHz,
                       TArray<double>& OutB, TArray<double>& OutA)
{
	if (Order < 1 || SampleRateHz <= 0.0)
	{
		return false;
	}
	const double Nyquist = SampleRateHz * 0.5;
	if (!(CutoffHz > 0.0 && CutoffHz < Nyquist))
	{
		return false;
	}

	// Unit-cutoff analog prototype poles, prewarped to the digital cutoff.
	const double Wa = 2.0 * SampleRateHz * FMath::Tan(UE_DOUBLE_PI * CutoffHz / SampleRateHz);
	TArray<FComplex> Poles;
	Poles.Reserve(Order);
	for (int32 k = 0; k < Order; ++k)
	{
		const double Theta = UE_DOUBLE_PI * (2.0 * k + Order + 1.0) / (2.0 * Order);
		Poles.Add(Wa * FComplex(FMath::Cos(Theta), FMath::Sin(Theta)));
	}

	// Bilinear transform.
	const double Fs2 = 2.0 * SampleRateHz;
	FComplex Denom(1.0, 0.0);
	TArray<FComplex> PolesZ;
	PolesZ.Reserve(Order);
	for (const FComplex& P : Poles)
	{
		PolesZ.Add((Fs2 + P) / (Fs2 - P));
		Denom *= (Fs2 - P);
	}

	const double Gain = FMath::Pow(Wa, static_cast<double>(Order));
	const double GainZ = Gain * (FComplex(1.0, 0.0) / Denom).real();

	TArray<FComplex> ZerosZ;
	ZerosZ.Init(FComplex(-1.0, 0.0), Order);

	PolyFromRoots(ZerosZ, OutB);
	PolyFromRoots(PolesZ, OutA);

	for (double& V : OutB)
	{
		V *= GainZ;
	}
	const double A0 = OutA[0];
	if (FMath::IsNearlyZero(A0))
	{
		return false;
	}
	for (double& V : OutB)
	{
		V /= A0;
	}
	for (double& V : OutA)
	{
		V /= A0;
	}
	return true;
}

void FiltFilt(const TArray<double>& B, const TArray<double>& A, TArray<double>& Values, int32 NumChannels)
{
	if (NumChannels <= 0)
	{
		return;
	}
	const int32 NumSamples = Values.Num() / NumChannels;
	if (NumSamples < 4)
	{
		return;
	}

	const int32 NTaps = FMath::Max(A.Num(), B.Num());
	const int32 Edge = FMath::Min(3 * NTaps, NumSamples - 1);
	const int32 Total = NumSamples + 2 * Edge;

	// Odd reflection about each endpoint: left[i] = 2*x[0] - x[edge-i], mirrored on the right.
	TArray<double> Ext;
	Ext.SetNumUninitialized(Total * NumChannels);
	for (int32 c = 0; c < NumChannels; ++c)
	{
		const double First = Values[c];
		const double Last = Values[(NumSamples - 1) * NumChannels + c];
		for (int32 i = 0; i < Edge; ++i)
		{
			Ext[i * NumChannels + c] = 2.0 * First - Values[(Edge - i) * NumChannels + c];
			Ext[(Edge + NumSamples + i) * NumChannels + c] =
				2.0 * Last - Values[(NumSamples - 2 - i) * NumChannels + c];
		}
		for (int32 i = 0; i < NumSamples; ++i)
		{
			Ext[(Edge + i) * NumChannels + c] = Values[i * NumChannels + c];
		}
	}

	TArray<double> Zi;
	LFilterZi(B, A, Zi);

	LFilter(B, A, Zi, Ext, NumChannels, Total, /*bReverse=*/false);
	LFilter(B, A, Zi, Ext, NumChannels, Total, /*bReverse=*/true);

	FMemory::Memcpy(Values.GetData(), Ext.GetData() + Edge * NumChannels,
	                sizeof(double) * NumSamples * NumChannels);
}

double GaussianSigmaForCutoff(double CutoffHz, double SampleRateHz)
{
	return 0.1325 * SampleRateHz / FMath::Max(CutoffHz, UE_DOUBLE_KINDA_SMALL_NUMBER);
}

void GaussianFilter(TArray<double>& Values, int32 NumChannels, double Sigma)
{
	if (NumChannels <= 0 || Sigma <= 0.0)
	{
		return;
	}
	const int32 NumSamples = Values.Num() / NumChannels;
	if (NumSamples < 2)
	{
		return;
	}

	const int32 Radius = FMath::Max(1, FMath::CeilToInt(4.0 * Sigma));
	TArray<double> Kernel;
	Kernel.SetNumUninitialized(2 * Radius + 1);
	double Sum = 0.0;
	for (int32 i = -Radius; i <= Radius; ++i)
	{
		const double V = FMath::Exp(-(static_cast<double>(i) * i) / (2.0 * Sigma * Sigma));
		Kernel[i + Radius] = V;
		Sum += V;
	}
	for (double& V : Kernel)
	{
		V /= Sum;
	}

	TArray<double> Src = Values;
	for (int32 s = 0; s < NumSamples; ++s)
	{
		for (int32 c = 0; c < NumChannels; ++c)
		{
			double Acc = 0.0;
			for (int32 k = -Radius; k <= Radius; ++k)
			{
				// Edge clamping, matching the reference's repeated-edge padding.
				const int32 Idx = FMath::Clamp(s + k, 0, NumSamples - 1);
				Acc += Kernel[k + Radius] * Src[Idx * NumChannels + c];
			}
			Values[s * NumChannels + c] = Acc;
		}
	}
}

void SmoothQuaternions(TArrayView<FQuat4f> Rotations, double CutoffHz, double SampleRateHz,
                       int32 Order, EMocapSmoothShapeInternal Shape)
{
	const int32 N = Rotations.Num();
	if (N < 4)
	{
		return;
	}

	// Sign continuity first: q and -q are the same rotation, but filtering the components of a
	// path that flips between them would smear across the flip.
	TArray<double> V;
	V.SetNumUninitialized(N * 4);
	double PrevX = Rotations[0].X, PrevY = Rotations[0].Y, PrevZ = Rotations[0].Z, PrevW = Rotations[0].W;
	V[0] = PrevX; V[1] = PrevY; V[2] = PrevZ; V[3] = PrevW;
	for (int32 i = 1; i < N; ++i)
	{
		double X = Rotations[i].X, Y = Rotations[i].Y, Z = Rotations[i].Z, W = Rotations[i].W;
		if (X * PrevX + Y * PrevY + Z * PrevZ + W * PrevW < 0.0)
		{
			X = -X; Y = -Y; Z = -Z; W = -W;
		}
		V[i * 4 + 0] = X; V[i * 4 + 1] = Y; V[i * 4 + 2] = Z; V[i * 4 + 3] = W;
		PrevX = X; PrevY = Y; PrevZ = Z; PrevW = W;
	}

	if (Shape == EMocapSmoothShapeInternal::Gaussian)
	{
		GaussianFilter(V, 4, GaussianSigmaForCutoff(CutoffHz, SampleRateHz));
	}
	else
	{
		TArray<double> B, A;
		if (!DesignButterworth(Order, CutoffHz, SampleRateHz, B, A))
		{
			return;
		}
		FiltFilt(B, A, V, 4);
	}

	for (int32 i = 0; i < N; ++i)
	{
		const double X = V[i * 4 + 0], Y = V[i * 4 + 1], Z = V[i * 4 + 2], W = V[i * 4 + 3];
		double Norm = FMath::Sqrt(X * X + Y * Y + Z * Z + W * W);
		if (Norm <= 0.0)
		{
			Norm = 1.0;
		}
		Rotations[i] = FQuat4f(static_cast<float>(X / Norm), static_cast<float>(Y / Norm),
		                       static_cast<float>(Z / Norm), static_cast<float>(W / Norm));
	}
}

void SmoothPositions(TArrayView<FVector3f> Positions, double CutoffHz, double SampleRateHz,
                     int32 Order, EMocapSmoothShapeInternal Shape)
{
	const int32 N = Positions.Num();
	if (N < 4)
	{
		return;
	}

	TArray<double> V;
	V.SetNumUninitialized(N * 3);
	for (int32 i = 0; i < N; ++i)
	{
		V[i * 3 + 0] = Positions[i].X;
		V[i * 3 + 1] = Positions[i].Y;
		V[i * 3 + 2] = Positions[i].Z;
	}

	if (Shape == EMocapSmoothShapeInternal::Gaussian)
	{
		GaussianFilter(V, 3, GaussianSigmaForCutoff(CutoffHz, SampleRateHz));
	}
	else
	{
		TArray<double> B, A;
		if (!DesignButterworth(Order, CutoffHz, SampleRateHz, B, A))
		{
			return;
		}
		FiltFilt(B, A, V, 3);
	}

	for (int32 i = 0; i < N; ++i)
	{
		Positions[i] = FVector3f(static_cast<float>(V[i * 3 + 0]), static_cast<float>(V[i * 3 + 1]),
		                         static_cast<float>(V[i * 3 + 2]));
	}
}

bool SelfTest()
{
	bool bOk = true;
	const double Fs = 60.0;

	// 1. slider mapping
	{
		const bool bGood = FMath::IsNearlyEqual(SliderToCutoffHz(7.0), 3.5, 1e-9)
			&& FMath::IsNearlyEqual(SliderToCutoffHz(10.0), 0.5, 1e-9)
			&& FMath::IsNearlyEqual(SliderToCutoffHz(0.5), 10.0, 1e-9);
		bOk &= bGood;
		UE_LOG(LogMocapSmooth, Display, TEXT("[MocapSmooth] selftest 1 slider mapping: %s"),
		       bGood ? TEXT("ok") : TEXT("FAIL"));
	}

	TArray<double> B, A;
	const bool bDesigned = DesignButterworth(2, 3.5, Fs, B, A);
	bOk &= bDesigned;

	// 2. overall -3 dB knee should sit at 0.802 * fc because filtfilt applies it twice
	if (bDesigned)
	{
		auto GainAt = [&](double Freq) -> double
		{
			const int32 N = 16384;
			TArray<double> X;
			X.SetNumUninitialized(N);
			for (int32 i = 0; i < N; ++i)
			{
				X[i] = FMath::Sin(2.0 * UE_DOUBLE_PI * Freq * i / Fs);
			}
			TArray<double> Y = X;
			FiltFilt(B, A, Y, 1);
			double NumSq = 0.0, DenSq = 0.0;
			for (int32 i = N / 4; i < 3 * N / 4; ++i)
			{
				NumSq += Y[i] * Y[i];
				DenSq += X[i] * X[i];
			}
			return FMath::Sqrt(NumSq / FMath::Max(DenSq, UE_DOUBLE_SMALL_NUMBER));
		};

		const double Expected = 0.802 * 3.5;
		double Best = 0.0, BestErr = TNumericLimits<double>::Max();
		for (double F = Expected - 0.6; F <= Expected + 0.6; F += 0.02)
		{
			const double Err = FMath::Abs(GainAt(F) - UE_DOUBLE_INV_SQRT_2);
			if (Err < BestErr)
			{
				BestErr = Err;
				Best = F;
			}
		}
		const bool bGood = FMath::Abs(Best - Expected) / Expected < 0.02;
		bOk &= bGood;
		UE_LOG(LogMocapSmooth, Display,
		       TEXT("[MocapSmooth] selftest 2 knee: measured %.3f Hz, expected %.3f Hz: %s"),
		       Best, Expected, bGood ? TEXT("ok") : TEXT("FAIL"));
	}

	// 3. zero phase: a symmetric input must stay symmetric
	if (bDesigned)
	{
		const int32 N = 513;
		TArray<double> X;
		X.SetNumZeroed(N);
		X[N / 2] = 1.0;
		FiltFilt(B, A, X, 1);
		double Asym = 0.0;
		for (int32 i = 0; i < N; ++i)
		{
			Asym = FMath::Max(Asym, FMath::Abs(X[i] - X[N - 1 - i]));
		}
		const bool bGood = Asym < 1e-9;
		bOk &= bGood;
		UE_LOG(LogMocapSmooth, Display, TEXT("[MocapSmooth] selftest 3 zero phase: asym %.2e: %s"),
		       Asym, bGood ? TEXT("ok") : TEXT("FAIL"));
	}

	// 4. Gaussian must never overshoot; 5. Butterworth is expected to ring ~3.4%
	{
		TArray<double> Step;
		Step.SetNumZeroed(400);
		for (int32 i = 200; i < 400; ++i)
		{
			Step[i] = 1.0;
		}

		TArray<double> G = Step;
		GaussianFilter(G, 1, 2.0);
		double GMax = 0.0;
		for (double V : G)
		{
			GMax = FMath::Max(GMax, V);
		}
		const bool bG = (GMax - 1.0) < 1e-12;
		bOk &= bG;
		UE_LOG(LogMocapSmooth, Display, TEXT("[MocapSmooth] selftest 4 gaussian overshoot %.2e: %s"),
		       GMax - 1.0, bG ? TEXT("ok") : TEXT("FAIL"));

		TArray<double> B2, A2;
		if (DesignButterworth(2, 3.6, Fs, B2, A2))
		{
			TArray<double> Bo = Step;
			FiltFilt(B2, A2, Bo, 1);
			double BMax = 0.0;
			for (double V : Bo)
			{
				BMax = FMath::Max(BMax, V);
			}
			const double Over = (BMax - 1.0) * 100.0;
			const bool bB = Over > 2.0 && Over < 5.0;
			bOk &= bB;
			UE_LOG(LogMocapSmooth, Display,
			       TEXT("[MocapSmooth] selftest 5 butterworth overshoot %.2f%%: %s"),
			       Over, bB ? TEXT("ok") : TEXT("FAIL"));
		}
	}

	// 6. quaternion smoothing keeps unit norm across a sign flip
	{
		const int32 N = 300;
		TArray<FQuat4f> Q;
		Q.SetNumUninitialized(N);
		for (int32 i = 0; i < N; ++i)
		{
			const float Ang = static_cast<float>(UE_DOUBLE_PI * 1.5 * i / (N - 1));
			FQuat4f Qi(FMath::Sin(Ang * 0.5f), 0.f, 0.f, FMath::Cos(Ang * 0.5f));
			if (i >= 150)
			{
				Qi = FQuat4f(-Qi.X, -Qi.Y, -Qi.Z, -Qi.W);
			}
			Q[i] = Qi;
		}
		SmoothQuaternions(Q, 3.5, Fs, 2, EMocapSmoothShapeInternal::Butterworth);
		double Err = 0.0;
		for (const FQuat4f& Qi : Q)
		{
			Err = FMath::Max<double>(Err, FMath::Abs(FMath::Sqrt(
				double(Qi.X) * Qi.X + double(Qi.Y) * Qi.Y + double(Qi.Z) * Qi.Z + double(Qi.W) * Qi.W) - 1.0));
		}
		const bool bGood = Err < 1e-6;
		bOk &= bGood;
		UE_LOG(LogMocapSmooth, Display, TEXT("[MocapSmooth] selftest 6 quat norm err %.2e: %s"),
		       Err, bGood ? TEXT("ok") : TEXT("FAIL"));
	}

	UE_LOG(LogMocapSmooth, Display, TEXT("[MocapSmooth] selftest: %s"),
	       bOk ? TEXT("ALL PASS") : TEXT("FAILURES ABOVE"));
	return bOk;
}

} // namespace MocapSmoothFilter
