/*=========================================================================
 *
 *  Copyright RTK Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/

#ifndef rtkOpenCLFFTRampImageFilter_hxx
#define rtkOpenCLFFTRampImageFilter_hxx

// Use local RTK FFTW files taken from Gaëtan Lehmann's code for
// thread safety: http://hdl.handle.net/10380/3154
#include <itkRealToHalfHermitianForwardFFTImageFilter.h>
#include <itkHalfHermitianToRealInverseFFTImageFilter.h>

#include <itkImageRegionIterator.h>
#include <itkImageRegionIteratorWithIndex.h>

#include <stdio.h>
#include <stdlib.h>

#include "OpenCLFFTFilter.h"

namespace rtk
{

	OpenCLFFTRampImageFilter
		::OpenCLFFTRampImageFilter() :
		m_HannCutFrequency(0.),
		m_CosineCutFrequency(0.),
		m_HammingFrequency(0.),
		m_HannCutFrequencyY(0.),
		m_RamLakCutFrequency(0.),
		m_SheppLoganCutFrequency(0.)
	{
		std::cout << "Ramp instantiation" << std::endl;
	}

	void
		OpenCLFFTRampImageFilter
		::GenerateInputRequestedRegion()
	{
		std::cout << "Ramp generate input" << std::endl;
		this->m_KernelDimension = (m_HannCutFrequencyY == 0.) ? 1 : 2;
		Superclass::GenerateInputRequestedRegion();
	}


	void
		OpenCLFFTRampImageFilter
		::UpdateFFTConvolutionKernel(const SizeType s)
	{
		std::cout << "Start function update" << std::endl;
		if (this->m_KernelFFT.GetPointer() != ITK_NULLPTR && s == this->m_PreviousKernelUpdateSize)
		{
			return;
		}
		m_PreviousKernelUpdateSize = s;

		const int width = s[0];
		const int height = s[1];

		// Allocate kernel
		SizeType size;
		size.Fill(1);
		size[0] = width;
		// FFTInputImagePointer
		itk::Image<float, 3>::Pointer kernel = itk::Image<float, 3>::New();
		kernel->SetRegions(size);
		kernel->Allocate();
		kernel->FillBuffer(0.);

		// Compute kernel in space domain (see Kak & Slaney, chapter 3 equation 61
		// page 72) although spacing is not squared according to equation 69 page 75
		double spacing = this->GetInput()->GetSpacing()[0];
		IndexType ix, jx;
		ix.Fill(0);
		jx.Fill(0);
		kernel->SetPixel(ix, 1. / (4.*spacing));
		for (ix[0] = 1, jx[0] = size[0] - 1; ix[0] < typename IndexType::IndexValueType(size[0] / 2); ix[0] += 2, jx[0] -= 2)
		{
			double v = ix[0] * vnl_math::pi;
			v = -1. / (v * v * spacing);
			kernel->SetPixel(ix, v);
			kernel->SetPixel(jx, v);
		}

		// FFT kernel
		//typedef itk::RealToHalfHermitianForwardFFTImageFilter< FFTInputImageType, FFTOutputImageType > FFTType;
		//typename FFTType::Pointer fftK = FFTType::New();
		//fftK->SetInput( kernel );
		//fftK->SetNumberOfThreads( this->GetNumberOfThreads() );
		//fftK->Update();
		std::cout << "Weeeee :)" << std::endl;
		OpenCLRealToHalfHermitianForwardFFTImageFilter(kernel, this->m_KernelFFT);
		//this->m_KernelFFT = fftK->GetOutput();

		// Windowing (if enabled)
		typedef itk::ImageRegionIteratorWithIndex<itk::Image<std::complex<float>, 3U>> IteratorType;
		IteratorType itK(this->m_KernelFFT, this->m_KernelFFT->GetLargestPossibleRegion());

		unsigned int n = this->m_KernelFFT->GetLargestPossibleRegion().GetSize(0);

		itK.GoToBegin();
		if (this->GetHannCutFrequency() > 0.)
		{
			const unsigned int ncut = itk::Math::Round<double>(n * vnl_math_min(1.0, this->GetHannCutFrequency()));
			for (unsigned int i = 0; i < ncut; i++, ++itK)
				itK.Set(itK.Get() * TFFTPrecision(0.5*(1 + vcl_cos(vnl_math::pi*i / ncut))));
		}
		else if (this->GetCosineCutFrequency() > 0.)
		{
			const unsigned int ncut = itk::Math::Round<double>(n * vnl_math_min(1.0, this->GetCosineCutFrequency()));
			for (unsigned int i = 0; i < ncut; i++, ++itK)
				itK.Set(itK.Get() * TFFTPrecision(vcl_cos(0.5*vnl_math::pi*i / ncut)));
		}
		else if (this->GetHammingFrequency() > 0.)
		{
			const unsigned int ncut = itk::Math::Round<double>(n * vnl_math_min(1.0, this->GetHammingFrequency()));
			for (unsigned int i = 0; i < ncut; i++, ++itK)
				itK.Set(itK.Get() * TFFTPrecision(0.54 + 0.46*(vcl_cos(vnl_math::pi*i / ncut))));
		}
		else if (this->GetRamLakCutFrequency() > 0.)
		{
			const unsigned int ncut = itk::Math::Round<double>(n * vnl_math_min(1.0, this->GetRamLakCutFrequency()));
			for (unsigned int i = 0; i < ncut; i++, ++itK) {}
		}
		else if (this->GetSheppLoganCutFrequency() > 0.)
		{
			const unsigned int ncut = itk::Math::Round<double>(n * vnl_math_min(1.0, this->GetSheppLoganCutFrequency()));
			//sinc(0) --> is 1
			++itK;
			for (unsigned int i = 1; i < ncut; i++, ++itK)
			{
				double x = 0.5*vnl_math::pi*i / ncut;
				itK.Set(itK.Get() * TFFTPrecision(vcl_sin(x) / x));
			}
		}
		else
		{
			itK.GoToReverseBegin();
			++itK;
		}

		for (; !itK.IsAtEnd(); ++itK)
		{
			itK.Set(itK.Get() * TFFTPrecision(0.));
		}

		// Replicate and window if required
		if (this->GetHannCutFrequencyY() > 0.)
		{
			size.Fill(1);
			size[0] = this->m_KernelFFT->GetLargestPossibleRegion().GetSize(0);
			size[1] = height;

			const unsigned int ncut = itk::Math::Round<double>((height / 2 + 1) * vnl_math_min(1.0, this->GetHannCutFrequencyY()));

			this->m_KernelFFT = FFTOutputImageType::New();
			this->m_KernelFFT->SetRegions(size);
			this->m_KernelFFT->Allocate();
			this->m_KernelFFT->FillBuffer(0.);

			IteratorType itTwoDK(this->m_KernelFFT, this->m_KernelFFT->GetLargestPossibleRegion());
			for (unsigned int j = 0; j < ncut; j++)
			{
				itK.GoToBegin();
				const TFFTPrecision win(0.5*(1 + vcl_cos(vnl_math::pi*j / ncut)));
				for (unsigned int i = 0; i < size[0]; ++itK, ++itTwoDK, i++)
				{
					itTwoDK.Set(win * itK.Get());
				}
			}
			itTwoDK.GoToReverseBegin();
			for (unsigned int j = 1; j < ncut; j++)
			{
				itK.GoToReverseBegin();
				const TFFTPrecision win(0.5*(1 + vcl_cos(vnl_math::pi*j / ncut)));
				for (unsigned int i = 0; i < size[0]; --itK, --itTwoDK, i++)
				{
					itTwoDK.Set(win * itK.Get());
				}
			}
		}
		this->m_KernelFFT->DisconnectPipeline();
	}

} // end namespace rtk
#endif
