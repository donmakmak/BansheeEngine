//********************************** Banshee Engine (www.banshee3d.com) **************************************************//
//**************** Copyright (c) 2016 Marko Pintera (marko.pintera@gmail.com). All rights reserved. **********************//
#include "BsIBLUtility.h"
#include "BsTexture.h"
#include "BsGpuParamsSet.h"
#include "BsRendererUtility.h"
#include "BsGpuBuffer.h"

namespace bs { namespace ct
{
	struct IBLUtility::Members
	{
		ReflectionCubeDownsampleMat downsampleMat;
		ReflectionCubeImportanceSampleMat importanceSampleMat;

		IrradianceComputeSHMat<3> shCompute3;
		IrradianceComputeSHMat<5> shCompute5;
		IrradianceReduceSHMat<3> shReduce3;
		IrradianceReduceSHMat<5> shReduce5;
		IrradianceProjectSHMat shProject5;
	};

	IBLUtility::Members* IBLUtility::m = nullptr;

	void IBLUtility::startUp()
	{
		m = bs_new<Members>();
	}

	void IBLUtility::shutDown()
	{
		bs_delete(m);
	}

	ReflectionCubeDownsampleParamDef gReflectionCubeDownsampleParamDef;

	ReflectionCubeDownsampleMat::ReflectionCubeDownsampleMat()
	{
		mParamBuffer = gReflectionCubeDownsampleParamDef.createBuffer();

		mParamsSet->setParamBlockBuffer("Input", mParamBuffer);
		mParamsSet->getGpuParams()->getTextureParam(GPT_FRAGMENT_PROGRAM, "gInputTex", mInputTexture);
	}

	void ReflectionCubeDownsampleMat::_initDefines(ShaderDefines& defines)
	{
		// Do nothing
	}

	void ReflectionCubeDownsampleMat::execute(const SPtr<Texture>& source, UINT32 face, const TextureSurface& surface, 
											 const SPtr<RenderTarget>& target)
	{
		mInputTexture.set(source, surface);
		gReflectionCubeDownsampleParamDef.gCubeFace.set(mParamBuffer, face);

		RenderAPI& rapi = RenderAPI::instance();
		rapi.setRenderTarget(target);

		gRendererUtility().setPass(mMaterial);
		gRendererUtility().setPassParams(mParamsSet);
		gRendererUtility().drawScreenQuad();
	}

	const UINT32 ReflectionCubeImportanceSampleMat::NUM_SAMPLES = 1024;
	ReflectionCubeImportanceSampleParamDef gReflectionCubeImportanceSampleParamDef;

	ReflectionCubeImportanceSampleMat::ReflectionCubeImportanceSampleMat()
	{
		mParamBuffer = gReflectionCubeImportanceSampleParamDef.createBuffer();

		mParamsSet->setParamBlockBuffer("Input", mParamBuffer);
		mParamsSet->getGpuParams()->getTextureParam(GPT_FRAGMENT_PROGRAM, "gInputTex", mInputTexture);
	}

	void ReflectionCubeImportanceSampleMat::_initDefines(ShaderDefines& defines)
	{
		defines.set("NUM_SAMPLES", NUM_SAMPLES); 
	}

	void ReflectionCubeImportanceSampleMat::execute(const SPtr<Texture>& source, UINT32 face, UINT32 mip, 
													const SPtr<RenderTarget>& target)
	{
		mInputTexture.set(source);
		gReflectionCubeImportanceSampleParamDef.gCubeFace.set(mParamBuffer, face);
		gReflectionCubeImportanceSampleParamDef.gMipLevel.set(mParamBuffer, mip);
		gReflectionCubeImportanceSampleParamDef.gNumMips.set(mParamBuffer, source->getProperties().getNumMipmaps() + 1);

		float width = (float)source->getProperties().getWidth();
		float height = (float)source->getProperties().getHeight();

		// First part of the equation for determining mip level to sample from.
		// See http://http.developer.nvidia.com/GPUGems3/gpugems3_ch20.html
		float mipFactor = 0.5f * std::log2(width * height / NUM_SAMPLES);
		gReflectionCubeImportanceSampleParamDef.gPrecomputedMipFactor.set(mParamBuffer, mipFactor);

		RenderAPI& rapi = RenderAPI::instance();
		rapi.setRenderTarget(target);

		gRendererUtility().setPass(mMaterial);
		gRendererUtility().setPassParams(mParamsSet);
		gRendererUtility().drawScreenQuad();
	}

	IrradianceComputeSHParamDef gIrradianceComputeSHParamDef;

	const static UINT32 TILE_WIDTH = 8;
	const static UINT32 TILE_HEIGHT = 8;
	const static UINT32 PIXELS_PER_THREAD = 4;

	template<int ORDER>
	IrradianceComputeSHMat<ORDER>::IrradianceComputeSHMat()
	{
		mParamBuffer = gIrradianceComputeSHParamDef.createBuffer();

		mParamsSet->setParamBlockBuffer("Params", mParamBuffer);

		SPtr<GpuParams> params = mParamsSet->getGpuParams();
		params->getTextureParam(GPT_COMPUTE_PROGRAM, "gInputTex", mInputTexture);
		params->getBufferParam(GPT_COMPUTE_PROGRAM, "gOutput", mOutputBuffer);
	}

	template<int ORDER>
	void IrradianceComputeSHMat<ORDER>::_initDefines(ShaderDefines& defines)
	{
		// TILE_WIDTH * TILE_HEIGHT must be pow2 because of parallel reduction algorithm
		defines.set("TILE_WIDTH", TILE_WIDTH);
		defines.set("TILE_HEIGHT", TILE_HEIGHT);

		// For very small textures this should be reduced so number of launched threads can properly utilize GPU cores
		defines.set("PIXELS_PER_THREAD", PIXELS_PER_THREAD);

		defines.set("ORDER", ORDER);
	}

	template<int ORDER>
	void IrradianceComputeSHMat<ORDER>::execute(const SPtr<Texture>& source, UINT32 face, const SPtr<GpuBuffer>& output)
	{
		auto& props = source->getProperties();
		UINT32 faceSize = props.getWidth();
		assert(faceSize == props.getHeight());

		Vector2I dispatchSize;
		dispatchSize.x = Math::divideAndRoundUp(faceSize, TILE_WIDTH * PIXELS_PER_THREAD);
		dispatchSize.y = Math::divideAndRoundUp(faceSize, TILE_HEIGHT * PIXELS_PER_THREAD);

		mInputTexture.set(source);
		gIrradianceComputeSHParamDef.gCubeFace.set(mParamBuffer, face);
		gIrradianceComputeSHParamDef.gFaceSize.set(mParamBuffer, source->getProperties().getWidth());
		gIrradianceComputeSHParamDef.gDispatchSize.set(mParamBuffer, dispatchSize);

		mOutputBuffer.set(output);

		RenderAPI& rapi = RenderAPI::instance();

		gRendererUtility().setComputePass(mMaterial);
		gRendererUtility().setPassParams(mParamsSet);
		rapi.dispatchCompute(dispatchSize.x, dispatchSize.y);
	}

	template<int ORDER>
	SPtr<GpuBuffer> IrradianceComputeSHMat<ORDER>::createOutputBuffer(const SPtr<Texture>& source, UINT32& numCoeffSets)
	{
		auto& props = source->getProperties();
		UINT32 faceSize = props.getWidth();
		assert(faceSize == props.getHeight());

		Vector2I dispatchSize;
		dispatchSize.x = Math::divideAndRoundUp(faceSize, TILE_WIDTH * PIXELS_PER_THREAD);
		dispatchSize.y = Math::divideAndRoundUp(faceSize, TILE_HEIGHT * PIXELS_PER_THREAD);

		numCoeffSets = dispatchSize.x * dispatchSize.y * 6;

		GPU_BUFFER_DESC bufferDesc;
		bufferDesc.type = GBT_STRUCTURED;
		bufferDesc.elementCount = numCoeffSets;
		bufferDesc.format = BF_UNKNOWN;
		bufferDesc.randomGpuWrite = true;

		if(ORDER == 3)
			bufferDesc.elementSize = sizeof(SHCoeffsAndWeight3);
		else
			bufferDesc.elementSize = sizeof(SHCoeffsAndWeight5);

		return GpuBuffer::create(bufferDesc);
	}

	template class IrradianceComputeSHMat<3>;
	template class IrradianceComputeSHMat<5>;

	IrradianceReduceSHParamDef gIrradianceReduceSHParamDef;

	template<int ORDER>
	IrradianceReduceSHMat<ORDER>::IrradianceReduceSHMat()
	{
		mParamBuffer = gIrradianceReduceSHParamDef.createBuffer();

		mParamsSet->setParamBlockBuffer("Params", mParamBuffer);

		SPtr<GpuParams> params = mParamsSet->getGpuParams();
		params->getBufferParam(GPT_COMPUTE_PROGRAM, "gInput", mInputBuffer);
		params->getBufferParam(GPT_COMPUTE_PROGRAM, "gOutput", mOutputBuffer);
	}

	template<int ORDER>
	void IrradianceReduceSHMat<ORDER>::_initDefines(ShaderDefines& defines)
	{
		defines.set("ORDER", ORDER);
	}

	template<int ORDER>
	void IrradianceReduceSHMat<ORDER>::execute(const SPtr<GpuBuffer>& source, UINT32 numCoeffSets, 
		const SPtr<GpuBuffer>& output, UINT32 outputIdx)
	{
		gIrradianceReduceSHParamDef.gNumEntries.set(mParamBuffer, numCoeffSets);
		gIrradianceReduceSHParamDef.gOutputIdx.set(mParamBuffer, outputIdx);

		mInputBuffer.set(source);
		mOutputBuffer.set(output);

		RenderAPI& rapi = RenderAPI::instance();

		gRendererUtility().setComputePass(mMaterial);
		gRendererUtility().setPassParams(mParamsSet);
		rapi.dispatchCompute(1);
	}

	template<int ORDER>
	SPtr<GpuBuffer> IrradianceReduceSHMat<ORDER>::createOutputBuffer(UINT32 numEntries)
	{
		GPU_BUFFER_DESC bufferDesc;
		bufferDesc.type = GBT_STRUCTURED;
		bufferDesc.elementCount = numEntries;
		bufferDesc.format = BF_UNKNOWN;
		bufferDesc.randomGpuWrite = true;

		if(ORDER == 3)
			bufferDesc.elementSize = sizeof(SHVector3RGB);
		else
			bufferDesc.elementSize = sizeof(SHVector5RGB);

		return GpuBuffer::create(bufferDesc);
	}

	template class IrradianceReduceSHMat<3>;
	template class IrradianceReduceSHMat<5>;

	IrradianceProjectSHParamDef gIrradianceProjectSHParamDef;

	IrradianceProjectSHMat::IrradianceProjectSHMat()
	{
		mParamBuffer = gIrradianceProjectSHParamDef.createBuffer();

		mParamsSet->setParamBlockBuffer("Params", mParamBuffer);

		SPtr<GpuParams> params = mParamsSet->getGpuParams();
		params->getBufferParam(GPT_FRAGMENT_PROGRAM, "gSHCoeffs", mInputBuffer);
	}

	void IrradianceProjectSHMat::_initDefines(ShaderDefines& defines)
	{
		// Do nothing
	}

	void IrradianceProjectSHMat::execute(const SPtr<GpuBuffer>& shCoeffs, UINT32 face, const SPtr<RenderTarget>& target)
	{
		gIrradianceProjectSHParamDef.gCubeFace.set(mParamBuffer, face);

		mInputBuffer.set(shCoeffs);

		RenderAPI& rapi = RenderAPI::instance();
		rapi.setRenderTarget(target);

		gRendererUtility().setPass(mMaterial);
		gRendererUtility().setPassParams(mParamsSet);
		gRendererUtility().drawScreenQuad();
	}

	const UINT32 IBLUtility::REFLECTION_CUBEMAP_SIZE = 256;
	const UINT32 IBLUtility::IRRADIANCE_CUBEMAP_SIZE = 32;

	void IBLUtility::filterCubemapForSpecular(const SPtr<Texture>& cubemap, const SPtr<Texture>& scratch)
	{
		auto& props = cubemap->getProperties();

		SPtr<Texture> scratchCubemap = scratch;
		if (scratchCubemap == nullptr)
		{
			TEXTURE_DESC cubemapDesc;
			cubemapDesc.type = TEX_TYPE_CUBE_MAP;
			cubemapDesc.format = props.getFormat();
			cubemapDesc.width = props.getWidth();
			cubemapDesc.height = props.getHeight();
			cubemapDesc.numMips = PixelUtil::getMaxMipmaps(cubemapDesc.width, cubemapDesc.height, 1, cubemapDesc.format);
			cubemapDesc.usage = TU_STATIC | TU_RENDERTARGET;

			scratchCubemap = Texture::create(cubemapDesc);
		}

		// We sample the cubemaps using importance sampling to generate roughness
		UINT32 numMips = props.getNumMipmaps();

		// Before importance sampling the cubemaps we first create box filtered versions for each mip level. This helps fix
		// the aliasing artifacts that would otherwise be noticeable on importance sampled cubemaps. The aliasing happens
		// because: 
		//  1. We use the same random samples for all pixels, which appears to duplicate reflections instead of creating
		//     noise, which is usually more acceptable 
		//  2. Even if we were to use fully random samples we would need a lot to avoid noticeable noise, which isn't
		//     practical

		// Copy base mip level to scratch cubemap
		for (UINT32 face = 0; face < 6; face++)
			cubemap->copy(scratchCubemap, face, 0, face, 0);

		// Fill out remaining scratch mip levels by downsampling
		for (UINT32 mip = 1; mip < numMips; mip++)
		{
			UINT32 sourceMip = mip - 1;
			downsampleCubemap(scratchCubemap, sourceMip, scratchCubemap, mip);
		}

		// Importance sample
		for (UINT32 mip = 1; mip < numMips; mip++)
		{
			for (UINT32 face = 0; face < 6; face++)
			{
				RENDER_TEXTURE_DESC cubeFaceRTDesc;
				cubeFaceRTDesc.colorSurfaces[0].texture = cubemap;
				cubeFaceRTDesc.colorSurfaces[0].face = face;
				cubeFaceRTDesc.colorSurfaces[0].numFaces = 1;
				cubeFaceRTDesc.colorSurfaces[0].mipLevel = mip;

				SPtr<RenderTarget> target = RenderTexture::create(cubeFaceRTDesc);

				m->importanceSampleMat.execute(scratchCubemap, face, mip, target);
			}
		}

		RenderAPI& rapi = RenderAPI::instance();
		rapi.setRenderTarget(nullptr);
	}

	void IBLUtility::filterCubemapForIrradiance(const SPtr<Texture>& cubemap, const SPtr<Texture>& output)
	{
		UINT32 numCoeffSets;
		SPtr<GpuBuffer> coeffSetBuffer = IrradianceComputeSHMat<5>::createOutputBuffer(cubemap, numCoeffSets);
		for (UINT32 face = 0; face < 6; face++)
			m->shCompute5.execute(cubemap, face, coeffSetBuffer);

		SPtr<GpuBuffer> coeffBuffer = IrradianceReduceSHMat<5>::createOutputBuffer(1);
		m->shReduce5.execute(coeffSetBuffer, numCoeffSets, coeffBuffer, 0);

		for (UINT32 face = 0; face < 6; face++)
		{
			RENDER_TEXTURE_DESC cubeFaceRTDesc;
			cubeFaceRTDesc.colorSurfaces[0].texture = output;
			cubeFaceRTDesc.colorSurfaces[0].face = face;
			cubeFaceRTDesc.colorSurfaces[0].numFaces = 1;
			cubeFaceRTDesc.colorSurfaces[0].mipLevel = 0;

			SPtr<RenderTarget> target = RenderTexture::create(cubeFaceRTDesc);
			m->shProject5.execute(coeffBuffer, face, target);
		}
	}

	void IBLUtility::filterCubemapForIrradiance(const SPtr<Texture>& cubemap, const SPtr<GpuBuffer>& output, 
		UINT32 outputIdx)
	{
		UINT32 numCoeffSets;
		SPtr<GpuBuffer> coeffSetBuffer = IrradianceComputeSHMat<3>::createOutputBuffer(cubemap, numCoeffSets);
		for (UINT32 face = 0; face < 6; face++)
			m->shCompute3.execute(cubemap, face, coeffSetBuffer);

		m->shReduce3.execute(coeffSetBuffer, numCoeffSets, output, outputIdx);
	}

	void IBLUtility::scaleCubemap(const SPtr<Texture>& src, UINT32 srcMip, const SPtr<Texture>& dst, UINT32 dstMip)
	{
		auto& srcProps = src->getProperties();
		auto& dstProps = dst->getProperties();

		SPtr<Texture> scratchTex = src;
		int sizeSrcLog2 = (int)log2((float)srcProps.getWidth());
		int sizeDstLog2 = (int)log2((float)dstProps.getWidth());

		int sizeLog2Diff = sizeSrcLog2 - sizeDstLog2;

		// If size difference is greater than one mip-level and we're downscaling, we need to generate intermediate mip
		// levels
		if(sizeLog2Diff > 1)
		{
			UINT32 mipSize = (UINT32)exp2((float)(sizeSrcLog2 - 1));
			UINT32 numDownsamples = sizeLog2Diff - 1;

			TEXTURE_DESC cubemapDesc;
			cubemapDesc.type = TEX_TYPE_CUBE_MAP;
			cubemapDesc.format = srcProps.getFormat();
			cubemapDesc.width = mipSize;
			cubemapDesc.height = mipSize;
			cubemapDesc.numMips = numDownsamples - 1;
			cubemapDesc.usage = TU_STATIC | TU_RENDERTARGET;

			scratchTex = Texture::create(cubemapDesc);

			downsampleCubemap(src, srcMip, scratchTex, 0);
			for(UINT32 i = 0; i < cubemapDesc.numMips; i++)
				downsampleCubemap(scratchTex, i, scratchTex, i + 1);

			srcMip = cubemapDesc.numMips;
		}

		// Same size so just copy
		if(sizeSrcLog2 == sizeDstLog2)
		{
			for (UINT32 face = 0; face < 6; face++)
				src->copy(dst, face, srcMip, face, dstMip);
		}
		else
			downsampleCubemap(scratchTex, srcMip, dst, dstMip);
	}

	void IBLUtility::downsampleCubemap(const SPtr<Texture>& src, UINT32 srcMip, const SPtr<Texture>& dst, UINT32 dstMip)
	{
		for (UINT32 face = 0; face < 6; face++)
		{
			RENDER_TEXTURE_DESC cubeFaceRTDesc;
			cubeFaceRTDesc.colorSurfaces[0].texture = dst;
			cubeFaceRTDesc.colorSurfaces[0].face = face;
			cubeFaceRTDesc.colorSurfaces[0].numFaces = 1;
			cubeFaceRTDesc.colorSurfaces[0].mipLevel = dstMip;

			SPtr<RenderTarget> target = RenderTexture::create(cubeFaceRTDesc);

			TextureSurface sourceSurface(srcMip, 1, 0, 6);
			m->downsampleMat.execute(src, face, sourceSurface, target);
		}
	}
}}
