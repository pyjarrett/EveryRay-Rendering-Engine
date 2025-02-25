#include "stdafx.h"
#include <iostream>

#include "ER_RenderingObject.h"
#include "ER_CoreComponent.h"
#include "ER_CoreException.h"
#include "ER_Core.h"
#include "ER_CoreTime.h"
#include "ER_Model.h"
#include "ER_Mesh.h"
#include "ER_Utility.h"
#include "ER_Illumination.h"
#include "ER_RenderableAABB.h"
#include "ER_Material.h"
#include "ER_Camera.h"
#include "ER_MatrixHelper.h"
#include "ER_Terrain.h"
#include "ER_Settings.h"
#include "ER_Scene.h"

namespace EveryRay_Core
{
	static int currentSplatChannnel = (int)TerrainSplatChannels::NONE;

	ER_RenderingObject::ER_RenderingObject(const std::string& pName, int index, ER_Core& pCore, ER_Camera& pCamera, std::unique_ptr<ER_Model> pModel, bool availableInEditor, bool isInstanced)
		:
		mCore(&pCore),
		mCamera(pCamera),
		mModel(std::move(pModel)),
		mMeshesReflectionFactors(0),
		mName(pName),
		mDebugGizmoAABB(nullptr),
		mAvailableInEditorMode(availableInEditor),
		mTransformationMatrix(XMMatrixIdentity()),
		mIsInstanced(isInstanced),
		mIndexInScene(index),
		mCurrentTextureQuality((RenderingObjectTextureQuality)ER_Settings::TexturesQuality)
	{
		if (!mModel)
		{
			std::string message = "Failed to create a RenderingObject from a model: ";
			message.append(pName);
			throw ER_CoreException(message.c_str());
		}

		mMeshesCount.push_back(0); // main LOD
		mMeshVertices.push_back({}); // main LOD
		mMeshAllVertices.push_back({}); // main LOD

		mMeshesCount[0] = mModel->Meshes().size();
		for (size_t i = 0; i < mMeshesCount[0]; i++)
		{
			mMeshVertices[0].push_back(mModel->GetMesh(i).Vertices());
			mMeshesTextureBuffers.push_back(TextureData());
			mMeshesReflectionFactors.push_back(0.0f);

			mCustomAlbedoTextures.push_back("");
			mCustomNormalTextures.push_back("");
			mCustomRoughnessTextures.push_back("");
			mCustomMetalnessTextures.push_back("");
			mCustomHeightTextures.push_back("");
			mCustomReflectionMaskTextures.push_back("");
		}

		for (size_t i = 0; i < mMeshVertices[0].size(); i++)
		{
			for (size_t j = 0; j < mMeshVertices[0][i].size(); j++)
			{
				mMeshAllVertices[0].push_back(mMeshVertices[0][i][j]);
			}
		}

		LoadAssignedMeshTextures();
		mLocalAABB = mModel->GenerateAABB();
		mGlobalAABB = mLocalAABB;

		if (mAvailableInEditorMode) {
			mDebugGizmoAABB = new ER_RenderableAABB(*mCore, XMFLOAT4{ 0.0f, 0.0f, 1.0f, 1.0f });
			mDebugGizmoAABB->InitializeGeometry({ mLocalAABB.first, mLocalAABB.second });
		}

		XMFLOAT4X4 transform = XMFLOAT4X4( mCurrentObjectTransformMatrix );
		mTransformationMatrix = XMLoadFloat4x4(&transform);

		mObjectConstantBuffer.Initialize(pCore.GetRHI(), "ER_RHI_GPUBuffer: Object's CB: " + mName);
	}

	ER_RenderingObject::~ER_RenderingObject()
	{
		DeleteObject(MeshMaterialVariablesUpdateEvent);

		for (auto& object : mMaterials)
			DeleteObject(object.second);
		mMaterials.clear();

		for (int lodI = 0; lodI < GetLODCount(); lodI++)
		{
			for (int meshI = 0; meshI < GetMeshCount(lodI); meshI++)
			{
				DeleteObject(mMeshRenderBuffers[lodI][meshI]);
			}
		}

		for (auto& meshesInstanceBuffersLOD : mMeshesInstanceBuffers)
			DeletePointerCollection(meshesInstanceBuffersLOD);
		mMeshesInstanceBuffers.clear();

		mMeshesTextureBuffers.clear();

		DeleteObject(mDebugGizmoAABB);
		DeleteObject(mInputPositionsOnTerrainBuffer);
		DeleteObject(mOutputPositionsOnTerrainBuffer);
		DeleteObjects(mTempInstancesPositions);

		mObjectConstantBuffer.Release();
	}

	void ER_RenderingObject::LoadMaterial(ER_Material* pMaterial, const std::string& materialName)
	{
		assert(pMaterial);
		mMaterials.emplace(materialName, pMaterial);
	}

	//from mesh-built-in textures (something that was specified in 3D tool, like Blender or Maya)
	void ER_RenderingObject::LoadAssignedMeshTextures()
	{
		auto pathBuilder = [&](std::wstring relativePath)
		{
			std::string fullPath;
			ER_Utility::GetDirectory(mModel->GetFileName(), fullPath);
			fullPath += "/";
			std::wstring resultPath;
			ER_Utility::ToWideString(fullPath, resultPath);
			resultPath += relativePath;
			return resultPath;
		};

		bool loadStatus = false;

		for (size_t i = 0; i < mMeshesCount[0]; i++)
		{
			if (mModel->GetMesh(i).GetMaterial().HasTexturesOfType(TextureType::TextureTypeDifffuse))
			{
				const std::vector<std::wstring>& texturesAlbedo = mModel->GetMesh(i).GetMaterial().GetTexturesByType(TextureType::TextureTypeDifffuse);
				if (texturesAlbedo.size() != 0)
				{
					std::wstring result = pathBuilder(texturesAlbedo.at(0));
					LoadTexture(&mMeshesTextureBuffers[i].AlbedoMap, &loadStatus,result, i);
				}
				else
					LoadTexture(&mMeshesTextureBuffers[i].AlbedoMap, &loadStatus, ER_Utility::GetFilePath(L"content\\textures\\emptyDiffuseMap.png"), i, true);
			}
			else
				LoadTexture(&mMeshesTextureBuffers[i].AlbedoMap, &loadStatus, ER_Utility::GetFilePath(L"content\\textures\\emptyDiffuseMap.png"), i, true);
			loadStatus = false;

			if (mModel->GetMesh(i).GetMaterial().HasTexturesOfType(TextureType::TextureTypeNormalMap))
			{
				const std::vector<std::wstring>& texturesNormal = mModel->GetMesh(i).GetMaterial().GetTexturesByType(TextureType::TextureTypeNormalMap);
				if (texturesNormal.size() != 0)
				{
					std::wstring result = pathBuilder(texturesNormal.at(0));
					LoadTexture(&mMeshesTextureBuffers[i].NormalMap, &loadStatus, result, i);
				}
				else
					LoadTexture(&mMeshesTextureBuffers[i].NormalMap, &loadStatus, ER_Utility::GetFilePath(L"content\\textures\\emptyNormalMap.jpg"), i, true);
			}
			else
				LoadTexture(&mMeshesTextureBuffers[i].NormalMap, &loadStatus, ER_Utility::GetFilePath(L"content\\textures\\emptyNormalMap.jpg"), i, true);
			loadStatus = false;

			if (mModel->GetMesh(i).GetMaterial().HasTexturesOfType(TextureType::TextureTypeSpecularMap))
			{
				const std::vector<std::wstring>& texturesSpec = mModel->GetMesh(i).GetMaterial().GetTexturesByType(TextureType::TextureTypeSpecularMap);
				if (texturesSpec.size() != 0)
				{
					std::wstring result = pathBuilder(texturesSpec.at(0));
					LoadTexture(&mMeshesTextureBuffers[i].RoughnessMap, &loadStatus, result, i);
				}
				else
					LoadTexture(&mMeshesTextureBuffers[i].RoughnessMap, &loadStatus, ER_Utility::GetFilePath(L"content\\textures\\emptyRoughnessMap.png"), i, true);
			}
			else
				LoadTexture(&mMeshesTextureBuffers[i].RoughnessMap, &loadStatus,ER_Utility::GetFilePath(L"content\\textures\\emptyRoughnessMap.png"), i, true);
			loadStatus = false;

			if (mModel->GetMesh(i).GetMaterial().HasTexturesOfType(TextureType::TextureTypeSpecularPowerMap))
			{
				const std::vector<std::wstring>& texturesMetallic = mModel->GetMesh(i).GetMaterial().GetTexturesByType(TextureType::TextureTypeSpecularPowerMap);
				if (texturesMetallic.size() != 0)
				{
					std::wstring result = pathBuilder(texturesMetallic.at(0));
					LoadTexture(&mMeshesTextureBuffers[i].MetallicMap, &loadStatus, result, i);
				}
				else
					LoadTexture(&mMeshesTextureBuffers[i].MetallicMap, &loadStatus,  ER_Utility::GetFilePath(L"content\\textures\\emptyMetallicMap.png"), i, true);
			}
			else
				LoadTexture(&mMeshesTextureBuffers[i].MetallicMap, &loadStatus, ER_Utility::GetFilePath(L"content\\textures\\emptyMetallicMap.png"), i, true);
			loadStatus = false;
		}
	}
	
	//from custom specified mesh textures in level file
	void ER_RenderingObject::LoadCustomMeshTextures(int meshIndex)
	{
		assert(meshIndex < mMeshesCount[0]);
		std::string errorMessage = mModel->GetFileName() + " of mesh index: " + std::to_string(meshIndex);

		bool loadStatus = false;

		if (!mCustomAlbedoTextures[meshIndex].empty())
			if (mCustomAlbedoTextures[meshIndex].back() != '\\')
				LoadTexture(&mMeshesTextureBuffers[meshIndex].AlbedoMap, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mCustomAlbedoTextures[meshIndex])), meshIndex);
		loadStatus = false;

		if (!mCustomNormalTextures[meshIndex].empty())
			if (mCustomNormalTextures[meshIndex].back() != '\\')
				LoadTexture(&mMeshesTextureBuffers[meshIndex].NormalMap, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mCustomNormalTextures[meshIndex])), meshIndex);
		loadStatus = false;

		if (!mCustomRoughnessTextures[meshIndex].empty())
			if (mCustomRoughnessTextures[meshIndex].back() != '\\')
				LoadTexture(&mMeshesTextureBuffers[meshIndex].RoughnessMap, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mCustomRoughnessTextures[meshIndex])), meshIndex);
		loadStatus = false;

		if (!mCustomMetalnessTextures[meshIndex].empty())
			if (mCustomMetalnessTextures[meshIndex].back() != '\\')
				LoadTexture(&mMeshesTextureBuffers[meshIndex].MetallicMap, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mCustomMetalnessTextures[meshIndex])), meshIndex);
		loadStatus = false;

		if (!mCustomHeightTextures[meshIndex].empty())
			if (mCustomHeightTextures[meshIndex].back() != '\\')
				LoadTexture(&mMeshesTextureBuffers[meshIndex].HeightMap, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mCustomHeightTextures[meshIndex])), meshIndex);
		loadStatus = false;

		if (!mCustomReflectionMaskTextures[meshIndex].empty())
			if (mCustomReflectionMaskTextures[meshIndex].back() != '\\')
				LoadTexture(&mMeshesTextureBuffers[meshIndex].ReflectionMaskMap, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mCustomReflectionMaskTextures[meshIndex])), meshIndex);
		loadStatus = false;

		//TODO
		//if (!extra2Path.empty())
		//TODO
		//if (!extra3Path.empty())
	}
	
	//from custom materials that the object has (snow, etc.); they are global for all meshes
	void ER_RenderingObject::LoadCustomMaterialTextures()
	{
		//snow
		bool loadStatus = false;
		if (!mSnowAlbedoTexturePath.empty())
			LoadTexture(&mSnowAlbedoTexture, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mSnowAlbedoTexturePath)), -1);
		loadStatus = false;
		if (!mSnowNormalTexturePath.empty())
			LoadTexture(&mSnowNormalTexture, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mSnowNormalTexturePath)), -1);
		loadStatus = false;
		if (!mSnowRoughnessTexturePath.empty())
			LoadTexture(&mSnowRoughnessTexture, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mSnowRoughnessTexturePath)), -1);
		loadStatus = false;

		//fur
		if (!mFurHeightTexturePath.empty())
			LoadTexture(&mFurHeightTexture, &loadStatus, ER_Utility::GetFilePath(ER_Utility::ToWideString(mFurHeightTexturePath)), -1);
		loadStatus = false;
	}

	// This is main method for loading textures before going to RHI
	// It supports quality levels, format check and different types of textures
	void ER_RenderingObject::LoadTexture(ER_RHI_GPUTexture** aTexture, bool* loadStat, const std::wstring& path, int meshIndex, bool isPlaceholder)
	{
		assert(loadStat);
		ER_RHI* rhi = mCore->GetRHI();

		const int extensionSymbolCount = 4; // .png, .dds, etc.
		const int texQualityCount = RenderingObjectTextureQuality::OBJECT_TEXTURE_COUNT;
		assert((int)mCurrentTextureQuality < texQualityCount);

		const wchar_t* postfixQuality[RenderingObjectTextureQuality::OBJECT_TEXTURE_COUNT] =
		{
			 L"_lq",
			 L"_mq",
			 L"_hq"
		};

		std::wstring possiblePaths[RenderingObjectTextureQuality::OBJECT_TEXTURE_COUNT];

		for (int i = 0; i < (int)RenderingObjectTextureQuality::OBJECT_TEXTURE_COUNT; i++)
		{
			possiblePaths[i] = path;
			possiblePaths[i].insert(path.length() - extensionSymbolCount, std::wstring(postfixQuality[i]));
		}

		const wchar_t* postfixDDS = L".dds";
		const wchar_t* postfixDDS_Capital = L".DDS";
		const wchar_t* postfixTGA = L".tga";
		const wchar_t* postfixTGA_Capital = L".TGA";
		bool ddsLoader = (path.substr(path.length() - extensionSymbolCount) == std::wstring(postfixDDS)) || (path.substr(path.length() - extensionSymbolCount) == std::wstring(postfixDDS_Capital));
		bool tgaLoader = (path.substr(path.length() - extensionSymbolCount) == std::wstring(postfixTGA)) || (path.substr(path.length() - extensionSymbolCount) == std::wstring(postfixTGA_Capital));
		std::string errorMessage = mModel->GetFileName() + " of mesh index: " + std::to_string(meshIndex);

		{
			bool didExist = false;
			//we start traversing through different texture quality levels unless we hit the first one
			for (int i = static_cast<int>(mCurrentTextureQuality); i >= 0; i--)
			{
				*aTexture = mCore->AddOrGetGPUTextureFromCache(possiblePaths[i], &didExist, false, true, loadStat, true);
				if (didExist)
					break;

				if (loadStat && !didExist)
				{
					if (*loadStat && *aTexture) // success
						break;

					if (!*loadStat && i <= 0) // after we traversed all possible levels, lets load the original path (maybe the texture does not have postfix)
						*aTexture = mCore->AddOrGetGPUTextureFromCache(path, &didExist);
				}

			}
			assert(*aTexture);

			if (!isPlaceholder/* && !didExist*/)
			{
				rhi->GenerateMipsWithTextureReplacement(aTexture,
					[this, aTexture](ER_RHI_GPUTexture** aNewTextureWithMips)
					{
						assert(*aNewTextureWithMips);
						assert(!(*aNewTextureWithMips)->debugName.empty());

						if (!mCore->IsGPUTextureInCache((*aNewTextureWithMips)->debugName))
							mCore->AddGPUTextureToCache((*aNewTextureWithMips)->debugName, *aNewTextureWithMips);

						*aTexture = mCore->AddOrGetGPUTextureFromCache((*aNewTextureWithMips)->debugName);
					}
				);
			}
		};
	}

	void ER_RenderingObject::LoadRenderBuffers(int lod)
	{
		assert(lod < GetLODCount());
		assert(mModel);
		ER_RHI* rhi = mCore->GetRHI();

		mMeshRenderBuffers.push_back({});
		assert(mMeshRenderBuffers.size() - 1 == lod);

		auto createIndexBuffer = [this, rhi](const ER_Mesh& aMesh, int meshIndex, int lod) {
			mMeshRenderBuffers[lod][meshIndex]->IndexBuffer = rhi->CreateGPUBuffer("ER_RHI_GPUBuffer: ER_RenderingObject - Index Buffer: " + mName + ", lod: " + std::to_string(lod) + ", mesh: " + std::to_string(meshIndex));
			aMesh.CreateIndexBuffer(mMeshRenderBuffers[lod][meshIndex]->IndexBuffer);
			mMeshRenderBuffers[lod][meshIndex]->IndicesCount = aMesh.Indices().size();
		};

		{
			for (size_t i = 0; i < mMeshesCount[lod]; i++)
			{
				mMeshRenderBuffers[lod].push_back(new RenderBufferData());
				mMeshRenderBuffers[lod][i]->VertexBuffer = rhi->CreateGPUBuffer("ER_RHI_GPUBuffer: ER_RenderingObject - Vertex Buffer: " + mName + ", lod: " + std::to_string(lod) + ", mesh: " + std::to_string(i));
				
				if (lod == 0)
					mModel->GetMesh(i).CreateVertexBuffer_PositionUvNormalTangent(mMeshRenderBuffers[lod][i]->VertexBuffer);
				else
					mModelLODs[lod - 1]->GetMesh(i).CreateVertexBuffer_PositionUvNormalTangent(mMeshRenderBuffers[lod][i]->VertexBuffer);

				createIndexBuffer((lod == 0) ? mModel->GetMesh(i) : mModelLODs[lod - 1]->GetMesh(i), i, lod);

				mMeshRenderBuffers[lod][i]->Stride = sizeof(VertexPositionTextureNormalTangent);
				mMeshRenderBuffers[lod][i]->Offset = 0;
			}
		}
	}
	
	void ER_RenderingObject::Draw(const std::string& materialName, bool toDepth, int meshIndex) {
		
		// for instanced objects we run DrawLOD() for all available LODs (some instances might end up in one LOD, others in other LODs)
		if (mIsInstanced)
		{
			for (int lod = 0; lod < GetLODCount(); lod++)
				DrawLOD(materialName, toDepth, meshIndex, lod);
		}
		else
			DrawLOD(materialName, toDepth, meshIndex, mCurrentLODIndex);
	}

	void ER_RenderingObject::DrawLOD(const std::string& materialName, bool toDepth, int meshIndex, int lod, bool skipCulling)
	{
		bool isForwardPass = materialName == ER_MaterialHelper::forwardLightingNonMaterialName && mIsForwardShading;

		ER_RHI* rhi = mCore->GetRHI();

		if (mMaterials.find(materialName) == mMaterials.end() && !isForwardPass)
			return;
		
		if (mIsRendered && (skipCulling || !mIsCulled) && mCurrentLODIndex != -1)
		{
			if (!isForwardPass && (!mMaterials.size() || mMeshRenderBuffers[lod].size() == 0))
				return;
			
			{
				mObjectConstantBuffer.Data.World = XMMatrixTranspose(mTransformationMatrix);
				
				if (mCore->GetLevel()->mIllumination)
				{
					mObjectConstantBuffer.Data.UseGlobalProbe = mUseIndirectGlobalLightProbe || (!mCore->GetLevel()->mLightProbesManager->IsEnabled() && mCore->GetLevel()->mLightProbesManager->AreGlobalProbesReady());
					mObjectConstantBuffer.Data.SkipIndirectProbeLighting = mCore->GetLevel()->mIllumination->IsSkippingIndirectRendering();
				}
				else
				{
					mObjectConstantBuffer.Data.UseGlobalProbe = true;
					mObjectConstantBuffer.Data.SkipIndirectProbeLighting = false;
				}

				mObjectConstantBuffer.Data.IndexOfRefraction = mIOR;
				mObjectConstantBuffer.Data.CustomRoughness = mCustomRoughness;
				mObjectConstantBuffer.Data.CustomMetalness = mCustomMetalness;
				mObjectConstantBuffer.ApplyChanges(rhi);

			}

			if (isForwardPass && mCore->GetLevel()->mIllumination)
				mCore->GetLevel()->mIllumination->PreparePipelineForForwardLighting(this);

			bool isSpecificMesh = (meshIndex != -1);
			for (int i = (isSpecificMesh) ? meshIndex : 0; i < ((isSpecificMesh) ? meshIndex + 1 : mMeshesCount[lod]); i++)
			{
				if (mIsInstanced)
					rhi->SetVertexBuffers({ mMeshRenderBuffers[lod][i]->VertexBuffer, mMeshesInstanceBuffers[lod][i]->InstanceBuffer });
				else
					rhi->SetVertexBuffers({ mMeshRenderBuffers[lod][i]->VertexBuffer });
				rhi->SetIndexBuffer(mMeshRenderBuffers[lod][i]->IndexBuffer);

				// run prepare callbacks for standard materials (specials are, i.e., shadow mapping, which are processed in their own systems)
				if (!isForwardPass && mMaterials[materialName]->IsStandard())
				{
					auto prepareMaterialBeforeRendering = MeshMaterialVariablesUpdateEvent->GetListener(materialName);
					if (prepareMaterialBeforeRendering)
						prepareMaterialBeforeRendering(i);
				}
				else if (isForwardPass && mCore->GetLevel()->mIllumination)
					mCore->GetLevel()->mIllumination->PrepareResourcesForForwardLighting(this, i);

				if (mIsInstanced)
				{
					if (mInstanceCountToRender[lod] > 0)
						rhi->DrawIndexedInstanced(mMeshRenderBuffers[lod][i]->IndicesCount, mInstanceCountToRender[lod], 0, 0, 0);
					else 
						continue;
				}
				else
					rhi->DrawIndexed(mMeshRenderBuffers[lod][i]->IndicesCount);
			}
		}
	}

	void ER_RenderingObject::DrawAABB(ER_RHI_GPUTexture* aRenderTarget, ER_RHI_GPUTexture* aDepth, ER_RHI_GPURootSignature* rs)
	{
		if (mIsSelected && mAvailableInEditorMode && mEnableAABBDebug && ER_Utility::IsEditorMode)
			mDebugGizmoAABB->Draw(aRenderTarget, aDepth, rs);
	}

	void ER_RenderingObject::SetTransformationMatrix(const XMMATRIX& mat)
	{
		mTransformationMatrix = mat;
		ER_MatrixHelper::GetFloatArray(mTransformationMatrix, mCurrentObjectTransformMatrix);
	}

	void ER_RenderingObject::SetTranslation(float x, float y, float z)
	{
		mTransformationMatrix *= XMMatrixTranslation(x, y, z);
		ER_MatrixHelper::GetFloatArray(mTransformationMatrix, mCurrentObjectTransformMatrix);
	}

	void ER_RenderingObject::SetScale(float x, float y, float z)
	{
		mTransformationMatrix *= XMMatrixScaling(x, y, z);
		ER_MatrixHelper::GetFloatArray(mTransformationMatrix, mCurrentObjectTransformMatrix);
	}

	void ER_RenderingObject::SetRotation(float x, float y, float z)
	{
		mTransformationMatrix *= XMMatrixRotationRollPitchYaw(x, y, z);
		ER_MatrixHelper::GetFloatArray(mTransformationMatrix, mCurrentObjectTransformMatrix);
	}

	// new instancing code
	void ER_RenderingObject::LoadInstanceBuffers(int lod)
	{
		auto rhi = mCore->GetRHI();
		assert(mModel != nullptr);
		assert(mIsInstanced == true);

		mInstanceData.push_back({});
		assert(lod < mInstanceData.size());

		mInstanceCountToRender.push_back(0);
		assert(lod == mInstanceCountToRender.size() - 1);

		mMeshesInstanceBuffers.push_back({});
		assert(lod == mMeshesInstanceBuffers.size() - 1);

		//adding extra instance data until we reach MAX_INSTANCE_COUNT 
		for (int i = mInstanceData[lod].size(); i < MAX_INSTANCE_COUNT; i++)
			AddInstanceData(XMMatrixIdentity(), lod);

		//mMeshesInstanceBuffers.clear();
		for (size_t i = 0; i < mMeshesCount[lod]; i++)
		{
			mMeshesInstanceBuffers[lod].push_back(new InstanceBufferData());
			mMeshesInstanceBuffers[lod][i]->InstanceBuffer = rhi->CreateGPUBuffer("ER_RHI_GPUBuffer: ER_RenderingObject - Instance Buffer: " + mName + ", lod: " + std::to_string(lod) + ", mesh: " + std::to_string(i));
			CreateInstanceBuffer(&mInstanceData[lod][0], MAX_INSTANCE_COUNT, mMeshesInstanceBuffers[lod][i]->InstanceBuffer);
			mMeshesInstanceBuffers[lod][i]->Stride = sizeof(InstancedData);
		}
	}
	// new instancing code
	void ER_RenderingObject::CreateInstanceBuffer(InstancedData* instanceData, UINT instanceCount, ER_RHI_GPUBuffer* instanceBuffer)
	{
		if (instanceCount > MAX_INSTANCE_COUNT)
			throw ER_CoreException("Instances count limit is exceeded!");

		assert(instanceBuffer);
		instanceBuffer->CreateGPUBufferResource(mCore->GetRHI(), instanceData, MAX_INSTANCE_COUNT, InstanceSize(), true, ER_BIND_VERTEX_BUFFER);
	}

	// new instancing code
	void ER_RenderingObject::UpdateInstanceBuffer(std::vector<InstancedData>& instanceData, int lod)
	{
		assert(lod < mMeshesInstanceBuffers.size());

		for (size_t i = 0; i < mMeshesCount[lod]; i++)
		{
			//CreateInstanceBuffer(instanceData);
			mInstanceCountToRender[lod] = instanceData.size();

			// dynamically update instance buffer
			mCore->GetRHI()->UpdateBuffer(mMeshesInstanceBuffers[lod][i]->InstanceBuffer, mInstanceCountToRender[lod] == 0 ? nullptr : &instanceData[0], InstanceSize() * mInstanceCountToRender[lod]);
		}
	}

	UINT ER_RenderingObject::InstanceSize() const
	{
		return sizeof(InstancedData);
	}

	void ER_RenderingObject::PerformCPUFrustumCull(ER_Camera* camera)
	{
		auto frustum = camera->GetFrustum();
		auto cullFunction = [frustum](ER_AABB& aabb) {
			bool culled = false;
			// start a loop through all frustum planes
			for (int planeID = 0; planeID < 6; ++planeID)
			{
				XMVECTOR planeNormal = XMVectorSet(frustum.Planes()[planeID].x, frustum.Planes()[planeID].y, frustum.Planes()[planeID].z, 0.0f);
				float planeConstant = frustum.Planes()[planeID].w;

				XMFLOAT3 axisVert;

				// x-axis
				if (frustum.Planes()[planeID].x > 0.0f)
					axisVert.x = aabb.first.x;
				else
					axisVert.x = aabb.second.x;

				// y-axis
				if (frustum.Planes()[planeID].y > 0.0f)
					axisVert.y = aabb.first.y;
				else
					axisVert.y = aabb.second.y;

				// z-axis
				if (frustum.Planes()[planeID].z > 0.0f)
					axisVert.z = aabb.first.z;
				else
					axisVert.z = aabb.second.z;


				if (XMVectorGetX(XMVector3Dot(planeNormal, XMLoadFloat3(&axisVert))) + planeConstant > 0.0f)
				{
					culled = true;
					// Skip remaining planes to check and move on 
					break;
				}
			}
			return culled;
		};

		assert(mInstanceCullingFlags.size() == mInstanceCount);

		if (mIsInstanced)
		{
			XMMATRIX instanceWorldMatrix = XMMatrixIdentity();
			const int currentLOD = 0; // no need to iterate through LODs (AABBs are shared between LODs, so culling results will be identical)

			mTempPostCullingInstanceData.clear();
			{
				std::vector<InstancedData> newInstanceData;
				for (int instanceIndex = 0; instanceIndex < static_cast<int>(mInstanceCount); instanceIndex++)
				{
					instanceWorldMatrix = XMLoadFloat4x4(&(mInstanceData[currentLOD][instanceIndex].World));
					mInstanceCullingFlags[instanceIndex] = cullFunction(mInstanceAABBs[instanceIndex]);
					if (!mInstanceCullingFlags[instanceIndex])
						newInstanceData.push_back(instanceWorldMatrix);
				}
				mTempPostCullingInstanceData = newInstanceData; //we store a copy for future usages

				//update every LOD group with new instance data after CPU frustum culling
				for (int lodIndex = 0; lodIndex < GetLODCount(); lodIndex++)
					UpdateInstanceBuffer(mTempPostCullingInstanceData, lodIndex);
			}
		}
		else
			mIsCulled = cullFunction(mGlobalAABB);
	}

	void ER_RenderingObject::StoreInstanceDataAfterTerrainPlacement()
	{
		assert(mTempInstancesPositions);
		XMMATRIX worldMatrix = XMMatrixIdentity();
		for (int lod = 0; lod < GetLODCount(); lod++)
		{
			for (int instanceI = 0; instanceI < static_cast<int>(mInstanceCount); instanceI++)
			{
				float scale = ER_Utility::RandomFloat(mTerrainProceduralObjectMinScale, mTerrainProceduralObjectMaxScale);
				float roll = ER_Utility::RandomFloat(mTerrainProceduralObjectMinRoll, mTerrainProceduralObjectMaxRoll);
				float pitch = ER_Utility::RandomFloat(mTerrainProceduralObjectMinPitch, mTerrainProceduralObjectMaxPitch);
				float yaw = ER_Utility::RandomFloat(mTerrainProceduralObjectMinYaw, mTerrainProceduralObjectMaxYaw);

				worldMatrix = XMMatrixScaling(scale, scale, scale) * XMMatrixRotationRollPitchYaw(pitch, yaw, roll);
				ER_MatrixHelper::SetTranslation(worldMatrix, XMFLOAT3(mTempInstancesPositions[instanceI].x, mTempInstancesPositions[instanceI].y, mTempInstancesPositions[instanceI].z));
				XMStoreFloat4x4(&(mInstanceData[lod][instanceI].World), worldMatrix);
				worldMatrix = XMMatrixIdentity();
			}
			UpdateInstanceBuffer(mInstanceData[lod], lod);
		}
	}

	XMFLOAT4 ER_RenderingObject::GetFurGravityStrength()
	{
		float time = static_cast<float>(mCore->GetCoreTotalTime());
		return XMFLOAT4{ sin(time * mFurWindFrequency) * mFurGravityDirection[0], sin(time * mFurWindFrequency) * mFurGravityDirection[1], sin(time * mFurWindFrequency) * mFurGravityDirection[2], mFurGravityStrength };
	}

	// Placement on terrain based on object's properties defined in level file (instance count, terrain splat, object scale variation, etc.)
	// This method is not supposed to run every frame, but during initialization or on request
	void ER_RenderingObject::PlaceProcedurallyOnTerrain(bool isOnInit)
	{
		auto rhi = mCore->GetRHI();

		ER_Terrain* terrain = mCore->GetLevel()->mTerrain;
		if (!terrain || !terrain->IsLoaded() || !mIsTerrainPlacement)
			return;

		if (!mIsInstanced)
		{
			XMFLOAT4 currentPos;
			ER_MatrixHelper::GetTranslation(XMLoadFloat4x4(&(XMFLOAT4X4(mCurrentObjectTransformMatrix))), currentPos);

			if (isOnInit)
			{
				DeleteObject(mInputPositionsOnTerrainBuffer);
				DeleteObject(mOutputPositionsOnTerrainBuffer);

				mInputPositionsOnTerrainBuffer = rhi->CreateGPUBuffer("ER_RHI_GPUBuffer: ER_RenderingObject on-terrain placement input positions buffer: " + mName);
				mInputPositionsOnTerrainBuffer->CreateGPUBufferResource(rhi, &currentPos, 1, sizeof(XMFLOAT4), false, ER_BIND_UNORDERED_ACCESS, 0, ER_RESOURCE_MISC_BUFFER_STRUCTURED);
				mOutputPositionsOnTerrainBuffer = rhi->CreateGPUBuffer("ER_RHI_GPUBuffer: ER_RenderingObject on-terrain placement output positions buffer: " + mName);
				mOutputPositionsOnTerrainBuffer->CreateGPUBufferResource(rhi, &currentPos, 1, sizeof(XMFLOAT4), false, ER_BIND_NONE, 0x10000L | 0x20000L /*legacy from DX11*/, ER_RESOURCE_MISC_BUFFER_STRUCTURED); //should be STAGING

				terrain->PlaceOnTerrain(mOutputPositionsOnTerrainBuffer, mInputPositionsOnTerrainBuffer, &currentPos, 1, (TerrainSplatChannels)mTerrainProceduralPlacementSplatChannel);
			
#ifndef ER_PLATFORM_WIN64_DX11
				std::string eventName = "On-terrain placement callback - initialization of ER_RenderingObject: " + mName;
				terrain->ReadbackPlacedPositionsOnInitEvent->AddListener(eventName, [&](ER_Terrain* aTerrain)
					{
						assert(aTerrain);
						XMFLOAT4 currentPos;
						aTerrain->ReadbackPlacedPositions(mOutputPositionsOnTerrainBuffer, mInputPositionsOnTerrainBuffer, &currentPos, 1);
						ER_MatrixHelper::SetTranslation(mTransformationMatrix, XMFLOAT3(currentPos.x, currentPos.y, currentPos.z));
						SetTransformationMatrix(mTransformationMatrix);
					}
				);
#else
				ER_MatrixHelper::SetTranslation(mTransformationMatrix, XMFLOAT3(currentPos.x, currentPos.y, currentPos.z));
				SetTransformationMatrix(mTransformationMatrix);
#endif
			}
			else
			{
				//TODO add support for non-init placement (during any time via editor)
			}
		}
		else
		{
			if (isOnInit)
			{
				DeleteObjects(mTempInstancesPositions);
				mTempInstancesPositions = new XMFLOAT4[mInstanceCount];

				for (int instanceI = 0; instanceI < static_cast<int>(mInstanceCount); instanceI++)
				{
					mTempInstancesPositions[instanceI] = XMFLOAT4(
						mTerrainProceduralZoneCenterPos.x + ER_Utility::RandomFloat(-mTerrainProceduralZoneRadius, mTerrainProceduralZoneRadius),
						mTerrainProceduralZoneCenterPos.y,
						mTerrainProceduralZoneCenterPos.z + ER_Utility::RandomFloat(-mTerrainProceduralZoneRadius, mTerrainProceduralZoneRadius), 1.0f);
				}

				DeleteObject(mInputPositionsOnTerrainBuffer);
				DeleteObject(mOutputPositionsOnTerrainBuffer);

				mInputPositionsOnTerrainBuffer = rhi->CreateGPUBuffer("ER_RHI_GPUBuffer: ER_RenderingObject on-terrain placement input positions buffer: " + mName);
				mInputPositionsOnTerrainBuffer->CreateGPUBufferResource(rhi, mTempInstancesPositions, mInstanceCount, sizeof(XMFLOAT4), false, ER_BIND_UNORDERED_ACCESS, 0, ER_RESOURCE_MISC_BUFFER_STRUCTURED);
				mOutputPositionsOnTerrainBuffer = rhi->CreateGPUBuffer("ER_RHI_GPUBuffer: ER_RenderingObject on-terrain placement input positions buffer: " + mName);
				mOutputPositionsOnTerrainBuffer->CreateGPUBufferResource(rhi, mTempInstancesPositions, mInstanceCount, sizeof(XMFLOAT4), false, ER_BIND_NONE, 0x10000L | 0x20000L /*legacy from DX11*/, ER_RESOURCE_MISC_BUFFER_STRUCTURED); //should be STAGING
				terrain->PlaceOnTerrain(mOutputPositionsOnTerrainBuffer, mInputPositionsOnTerrainBuffer, mTempInstancesPositions, mInstanceCount, (TerrainSplatChannels)mTerrainProceduralPlacementSplatChannel);
				
#ifndef ER_PLATFORM_WIN64_DX11
				std::string eventName = "On-terrain placement callback - initialization of ER_RenderingObject: " + mName;
				terrain->ReadbackPlacedPositionsOnInitEvent->AddListener(eventName, [&](ER_Terrain* aTerrain)
					{
						assert(aTerrain);
						aTerrain->ReadbackPlacedPositions(mOutputPositionsOnTerrainBuffer, mInputPositionsOnTerrainBuffer, mTempInstancesPositions, mInstanceCount);
						StoreInstanceDataAfterTerrainPlacement();
					}
				);
#else
				StoreInstanceDataAfterTerrainPlacement();
#endif
			}
			else
			{
				//TODO add support for non-init placement (during any time via editor)
			}
		}
		mIsTerrainPlacementFinished = true;
	}
	void ER_RenderingObject::Update(const ER_CoreTime& time)
	{
		ER_Camera* camera = (ER_Camera*)(mCore->GetServices().FindService(ER_Camera::TypeIdClass()));
		assert(camera);

		bool editable = ER_Utility::IsEditorMode && mAvailableInEditorMode && mIsSelected;

		if (editable && mIsInstanced)
		{
			// load current selected instance's transform to temp transform (for UI)
			ER_MatrixHelper::GetFloatArray(mInstanceData[0][mEditorSelectedInstancedObjectIndex].World, mCurrentObjectTransformMatrix);
		}

		// place procedurally on terrain (only executed once, on load)
		//if (mIsTerrainPlacement && !mIsTerrainPlacementFinished)
		//	PlaceProcedurallyOnTerrain();

		//update AABBs (global and instanced)
		{
			mGlobalAABB = mLocalAABB;
			UpdateAABB(mGlobalAABB, mTransformationMatrix);

			if (mIsInstanced)
			{
				XMMATRIX instanceWorldMatrix = XMMatrixIdentity();
				for (int instanceIndex = 0; instanceIndex < static_cast<int>(mInstanceCount); instanceIndex++)
				{
					instanceWorldMatrix = XMLoadFloat4x4(&(mInstanceData[0][instanceIndex].World));
					mInstanceAABBs[instanceIndex] = mLocalAABB;
					UpdateAABB(mInstanceAABBs[instanceIndex], instanceWorldMatrix);
				}
			}
		}

		if (ER_Utility::IsMainCameraCPUFrustumCulling && camera)
			PerformCPUFrustumCull(camera);
		else
		{
			if (mIsInstanced)
			{
				//just updating transforms (that could be changed in a previous frame); this is not optimal (GPU buffer map() every frame...)
				for (int lod = 0; lod < GetLODCount(); lod++)
					UpdateInstanceBuffer(mInstanceData[lod], lod);
			}
		}

		if (GetLODCount() > 1)
			UpdateLODs();

		if (editable)
		{
			UpdateGizmos();
			ShowInstancesListWindow();
			if (mEnableAABBDebug)
				mDebugGizmoAABB->Update(mGlobalAABB);
		}
	}

	void ER_RenderingObject::UpdateAABB(ER_AABB& aabb, const XMMATRIX& transformMatrix)
	{
		// computing AABB from the non-axis aligned BB
		mCurrentGlobalAABBVertices[0] = (XMFLOAT3(aabb.first.x, aabb.second.y, aabb.first.z));
		mCurrentGlobalAABBVertices[1] = (XMFLOAT3(aabb.second.x, aabb.second.y, aabb.first.z));
		mCurrentGlobalAABBVertices[2] = (XMFLOAT3(aabb.second.x, aabb.first.y, aabb.first.z));
		mCurrentGlobalAABBVertices[3] = (XMFLOAT3(aabb.first.x, aabb.first.y, aabb.first.z));
		mCurrentGlobalAABBVertices[4] = (XMFLOAT3(aabb.first.x, aabb.second.y, aabb.second.z));
		mCurrentGlobalAABBVertices[5] = (XMFLOAT3(aabb.second.x, aabb.second.y, aabb.second.z));
		mCurrentGlobalAABBVertices[6] = (XMFLOAT3(aabb.second.x, aabb.first.y, aabb.second.z));
		mCurrentGlobalAABBVertices[7] = (XMFLOAT3(aabb.first.x, aabb.first.y, aabb.second.z));

		// non-axis-aligned BB (applying transform)
		for (size_t i = 0; i < 8; i++)
		{
			XMVECTOR point = XMVector3Transform(XMLoadFloat3(&(mCurrentGlobalAABBVertices[i])), transformMatrix);
			XMStoreFloat3(&(mCurrentGlobalAABBVertices[i]), point);
		}

		XMFLOAT3 minVertex = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
		XMFLOAT3 maxVertex = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

		for (UINT i = 0; i < 8; i++)
		{
			//Get the smallest vertex 
			minVertex.x = std::min(minVertex.x, mCurrentGlobalAABBVertices[i].x);    // Find smallest x value in model
			minVertex.y = std::min(minVertex.y, mCurrentGlobalAABBVertices[i].y);    // Find smallest y value in model
			minVertex.z = std::min(minVertex.z, mCurrentGlobalAABBVertices[i].z);    // Find smallest z value in model

			//Get the largest vertex 
			maxVertex.x = std::max(maxVertex.x, mCurrentGlobalAABBVertices[i].x);    // Find largest x value in model
			maxVertex.y = std::max(maxVertex.y, mCurrentGlobalAABBVertices[i].y);    // Find largest y value in model
			maxVertex.z = std::max(maxVertex.z, mCurrentGlobalAABBVertices[i].z);    // Find largest z value in model
		}

		aabb = ER_AABB(minVertex, maxVertex);
	}
	
	void ER_RenderingObject::UpdateGizmos()
	{
		if (!(mAvailableInEditorMode && mIsSelected))
			return;

		ER_MatrixHelper::GetFloatArray(mCamera.ViewMatrix4X4(), mCameraViewMatrix);
		ER_MatrixHelper::GetFloatArray(mCamera.ProjectionMatrix4X4(), mCameraProjectionMatrix);

		ShowObjectsEditorWindow(mCameraViewMatrix, mCameraProjectionMatrix, mCurrentObjectTransformMatrix);

		XMFLOAT4X4 mat(mCurrentObjectTransformMatrix);
		mTransformationMatrix = XMLoadFloat4x4(&mat);

		//update instance world transform (from editor's gizmo/UI)
		if (mIsInstanced && ER_Utility::IsEditorMode)
		{
			for (int lod = 0; lod < GetLODCount(); lod++)
				mInstanceData[lod][mEditorSelectedInstancedObjectIndex].World = XMFLOAT4X4(mCurrentObjectTransformMatrix);
		}
	}
	
	// Shows and updates an ImGui/ImGizmo window for objects editor.
	// You can edit transform (via UI or via gizmo), you can move to object, set/read some flags, get some useful info, etc.
	void ER_RenderingObject::ShowObjectsEditorWindow(const float *cameraView, float *cameraProjection, float* matrix)
	{
		static ImGuizmo::OPERATION mCurrentGizmoOperation(ImGuizmo::TRANSLATE);
		static ImGuizmo::MODE mCurrentGizmoMode(ImGuizmo::WORLD);
		static bool useSnap = false;
		static float snap[3] = { 1.f, 1.f, 1.f };
		static float bounds[] = { -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
		static float boundsSnap[] = { 0.1f, 0.1f, 0.1f };
		static bool boundSizing = false;
		static bool boundSizingSnap = false;

		if (ER_Utility::IsEditorMode) {

			ER_Utility::IsLightEditor = false;
			ER_Utility::IsFoliageEditor = false;

			ImGui::Begin("Object Editor");
			std::string name = mName;
			if (mIsInstanced)
			{
				name = mInstancesNames[mEditorSelectedInstancedObjectIndex];
				if (mInstanceCullingFlags[mEditorSelectedInstancedObjectIndex]) //showing info for main LOD only in editor
					name += " (Culled)";
			}
			else
			{
				name += " LOD #" + std::to_string(mCurrentLODIndex);
				if (mIsCulled)
					name += " (Culled)";
			}

			ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.24f, 1), name.c_str());

			ImGui::Separator();
			std::string lodCountText = "* LOD count: " + std::to_string(GetLODCount());
			ImGui::Text(lodCountText.c_str());
			for (int lodI = 0; lodI < GetLODCount(); lodI++)
			{
				std::string vertexCountText = "--> Vertex count LOD#" + std::to_string(lodI) + ": " + std::to_string(GetVertices(lodI).size());
				ImGui::Text(vertexCountText.c_str());
			}

			std::string meshCountText = "* Mesh count: " + std::to_string(GetMeshCount());
			ImGui::Text(meshCountText.c_str());

			std::string instanceCountText = "* Instance count: " + std::to_string(GetInstanceCount());
			ImGui::Text(instanceCountText.c_str());

			std::string shadingModeName = "* Shaded in: ";
			if (mIsForwardShading)
				shadingModeName += "Forward";
			else 
				shadingModeName += "Deferred";
			ImGui::Text(shadingModeName.c_str());

			ImGui::Separator();
			ImGui::Checkbox("Rendered", &mIsRendered);
			ImGui::Checkbox("Show AABB", &mEnableAABBDebug);
			ImGui::Checkbox("Show Wireframe", &mWireframeMode);
			if (ImGui::Button("Move camera to"))
			{
				XMFLOAT3 newCameraPos;
				ER_MatrixHelper::GetTranslation(XMLoadFloat4x4(&(XMFLOAT4X4(mCurrentObjectTransformMatrix))), newCameraPos);

				ER_Camera* camera = (ER_Camera*)(mCore->GetServices().FindService(ER_Camera::TypeIdClass()));
				if (camera)
					camera->SetPosition(newCameraPos);
			}

			//terrain
			//{
			//	ImGui::Combo("Terrain splat channel", &currentSplatChannnel, DisplayedSplatChannnelNames, 5);
			//	TerrainSplatChannels currentChannel = (TerrainSplatChannels)currentSplatChannnel;
			//	ER_Terrain* terrain = mCore->GetLevel()->mTerrain;
			//
			//	if (ImGui::Button("Place on terrain") && terrain && terrain->IsLoaded())
			//	{
			//		XMFLOAT4 currentPos;
			//		ER_MatrixHelper::GetTranslation(XMLoadFloat4x4(&(XMFLOAT4X4(mCurrentObjectTransformMatrix))), currentPos);
			//		terrain->PlaceOnTerrain(&currentPos, 1, currentChannel);
			//
			//		mMatrixTranslation[0] = currentPos.x;
			//		mMatrixTranslation[1] = currentPos.y;
			//		mMatrixTranslation[2] = currentPos.z;
			//		ImGuizmo::RecomposeMatrixFromComponents(mMatrixTranslation, mMatrixRotation, mMatrixScale, matrix);
			//	}
			//}

			ImGui::SliderFloat("Custom roughness", &mCustomRoughness, -1.0f, 1.0f);
			ImGui::SliderFloat("Custom metalness", &mCustomMetalness, -1.0f, 1.0f);
			if (mIsTransparent)
				ImGui::SliderFloat("IOR", &mIOR, -5.0f, 5.0f);
			if (mFurLayersCount > 0)
			{
				ImGui::ColorEdit3("Fur Color", mFurColor);
				ImGui::SliderFloat("Fur Interpolation with albedo", &mFurColorInterpolation, 0.0, 1.0);
				ImGui::SliderFloat("Fur Length", &mFurLength, 0.01f, 25.0f);
				ImGui::SliderFloat("Fur Cutoff", &mFurCutoff, 0.01f, 1.0f);
				ImGui::SliderFloat("Fur Cutoff End", &mFurCutoffEnd, 0.01f, 1.0f);
				ImGui::SliderFloat("Fur UV Scale", &mFurUVScale, 0.01f, 15.0f);
				ImGui::SliderFloat3("Fur Gravity Dir", &mFurGravityDirection[0], -1.0, 1.0);
				ImGui::SliderFloat("Fur Gravity Strength", &mFurGravityStrength, 0.0, 10.0);
				ImGui::SliderFloat("Fur Wind Frequency", &mFurWindFrequency, 0.0, 10.0);
			}

			//Transforms
			if (ImGui::IsKeyPressed(84))
				mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
			if (ImGui::IsKeyPressed(89))
				mCurrentGizmoOperation = ImGuizmo::ROTATE;
			if (ImGui::IsKeyPressed(82)) // r Key
				mCurrentGizmoOperation = ImGuizmo::SCALE;

			if (ImGui::RadioButton("Translate", mCurrentGizmoOperation == ImGuizmo::TRANSLATE))
				mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
			ImGui::SameLine();
			if (ImGui::RadioButton("Rotate", mCurrentGizmoOperation == ImGuizmo::ROTATE))
				mCurrentGizmoOperation = ImGuizmo::ROTATE;
			ImGui::SameLine();
			if (ImGui::RadioButton("Scale", mCurrentGizmoOperation == ImGuizmo::SCALE))
				mCurrentGizmoOperation = ImGuizmo::SCALE;


			ImGuizmo::DecomposeMatrixToComponents(matrix, mMatrixTranslation, mMatrixRotation, mMatrixScale);
			ImGui::InputFloat3("Tr", mMatrixTranslation, 3);
			ImGui::InputFloat3("Rt", mMatrixRotation, 3);
			ImGui::InputFloat3("Sc", mMatrixScale, 3);
			ImGuizmo::RecomposeMatrixFromComponents(mMatrixTranslation, mMatrixRotation, mMatrixScale, matrix);
			ImGui::End();

			ImGuiIO& io = ImGui::GetIO();
			ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
			ImGuizmo::Manipulate(cameraView, cameraProjection, mCurrentGizmoOperation, mCurrentGizmoMode, matrix, NULL, useSnap ? &snap[0] : NULL, boundSizing ? bounds : NULL, boundSizingSnap ? boundsSnap : NULL);
		}
	}
	
	// Shows an ImGui window for instances list.
	// You can select an instance, read some useful info about it and edit it via "Objects Editor".
	// We do not need to edit per LOD (LODs share same transforms, AABBs, names).
	void ER_RenderingObject::ShowInstancesListWindow()
	{
		if (!(mAvailableInEditorMode && mIsSelected && mIsInstanced))
			return;

		assert(mInstanceCount != 0);
		assert(mInstanceData[0].size() != 0 && mInstanceData[0].size() == mInstanceCount);
		assert(mInstancesNames.size() == mInstanceCount);

		std::string title = mName + " instances:";
		ImGui::Begin(title.c_str());

		for (int i = 0; i < mInstanceData[0].size(); i++) {
			mInstancedNamesUI[i] = mInstancesNames[i].c_str();
		}

		ImGui::PushItemWidth(-1);
		ImGui::ListBox("##empty", &mEditorSelectedInstancedObjectIndex, mInstancedNamesUI, mInstanceData[0].size(), 15);
		ImGui::End();
	}
	
	void ER_RenderingObject::LoadLOD(std::unique_ptr<ER_Model> pModel)
	{
		mMeshesCount.push_back(pModel->Meshes().size());
		mModelLODs.push_back(std::move(pModel));
		mMeshVertices.push_back({});
		mMeshAllVertices.push_back({}); 

		int lodIndex = mMeshesCount.size() - 1;

		for (size_t i = 0; i < mMeshesCount[lodIndex]; i++)
			mMeshVertices[lodIndex].push_back(mModelLODs[lodIndex - 1]->GetMesh(i).Vertices());

		for (size_t i = 0; i < mMeshVertices[lodIndex].size(); i++)
		{
			for (size_t j = 0; j < mMeshVertices[lodIndex][i].size(); j++)
			{
				mMeshAllVertices[lodIndex].push_back(mMeshVertices[lodIndex][i][j]);
			}
		}

		LoadRenderBuffers(lodIndex);
	}

	void ER_RenderingObject::ResetInstanceData(int count, bool clear, int lod)
	{
		assert(lod < GetLODCount());

		mInstanceCountToRender[lod] = count;

		if (lod == 0)
		{
			mInstanceCount = count;
			for (int i = 0; i < static_cast<int>(mInstanceCount); i++)
			{
				std::string instanceName = mName + " #" + std::to_string(i);
				mInstancesNames.push_back(instanceName);
				mInstanceAABBs.push_back(mLocalAABB);
				mInstanceCullingFlags.push_back(false);
			}
		}

		if (clear)
			mInstanceData[lod].clear();
	}
	void ER_RenderingObject::AddInstanceData(const XMMATRIX& worldMatrix, int lod)
	{
		if (lod == -1) {
			for (int lod = 0; lod < GetLODCount(); lod++)
				mInstanceData[lod].push_back(InstancedData(worldMatrix));
			return;
		}

		assert(lod < mInstanceData.size());
		mInstanceData[lod].push_back(InstancedData(worldMatrix));
	}

	void ER_RenderingObject::UpdateLODs()
	{
		if (mIsInstanced) {
			if (!ER_Utility::IsMainCameraCPUFrustumCulling && mInstanceData.size() == 0)
				return;
			if (ER_Utility::IsMainCameraCPUFrustumCulling && mTempPostCullingInstanceData.size() == 0)
				return;

			mTempPostLoddingInstanceData.clear();
			for (int lod = 0; lod < GetLODCount(); lod++)
				mTempPostLoddingInstanceData.push_back({});

			//traverse through original or culled instance data (sort of "read-only") to rebalance LOD's instance buffers
			int length = (ER_Utility::IsMainCameraCPUFrustumCulling) ? static_cast<int>(mTempPostCullingInstanceData.size()) : static_cast<int>(mInstanceData[0].size());
			for (int i = 0; i < length; i++)
			{
				XMFLOAT3 pos;
				XMMATRIX mat = (ER_Utility::IsMainCameraCPUFrustumCulling) ? XMLoadFloat4x4(&mTempPostCullingInstanceData[i].World) : XMLoadFloat4x4(&mInstanceData[0][i].World);
				ER_MatrixHelper::GetTranslation(mat, pos);

				float distanceToCameraSqr =
					(mCamera.Position().x - pos.x) * (mCamera.Position().x - pos.x) +
					(mCamera.Position().y - pos.y) * (mCamera.Position().y - pos.y) +
					(mCamera.Position().z - pos.z) * (mCamera.Position().z - pos.z);

				//XMMATRIX newMat;
				if (distanceToCameraSqr <= ER_Utility::DistancesLOD[0] * ER_Utility::DistancesLOD[0]) {
					mTempPostLoddingInstanceData[0].push_back((ER_Utility::IsMainCameraCPUFrustumCulling) ? mTempPostCullingInstanceData[i].World : mInstanceData[0][i].World);
				}
				else if (ER_Utility::DistancesLOD[0] * ER_Utility::DistancesLOD[0] < distanceToCameraSqr && distanceToCameraSqr <= ER_Utility::DistancesLOD[1] * ER_Utility::DistancesLOD[1]) {
					mTempPostLoddingInstanceData[1].push_back((ER_Utility::IsMainCameraCPUFrustumCulling) ? mTempPostCullingInstanceData[i].World : mInstanceData[0][i].World);
				}
				else if (ER_Utility::DistancesLOD[1] * ER_Utility::DistancesLOD[1] < distanceToCameraSqr && distanceToCameraSqr <= ER_Utility::DistancesLOD[2] * ER_Utility::DistancesLOD[2]) {
					mTempPostLoddingInstanceData[2].push_back((ER_Utility::IsMainCameraCPUFrustumCulling) ? mTempPostCullingInstanceData[i].World : mInstanceData[0][i].World);
				}
			}

			for (int i = 0; i < GetLODCount(); i++)
				UpdateInstanceBuffer(mTempPostLoddingInstanceData[i], i);
		}
		else
		{
			XMFLOAT3 pos;
			ER_MatrixHelper::GetTranslation(mTransformationMatrix, pos);

			float distanceToCameraSqr =
				(mCamera.Position().x - pos.x) * (mCamera.Position().x - pos.x) +
				(mCamera.Position().y - pos.y) * (mCamera.Position().y - pos.y) +
				(mCamera.Position().z - pos.z) * (mCamera.Position().z - pos.z);

			if (distanceToCameraSqr <= ER_Utility::DistancesLOD[0] * ER_Utility::DistancesLOD[0]) {
				mCurrentLODIndex = 0;
			}
			else if (ER_Utility::DistancesLOD[0] * ER_Utility::DistancesLOD[0] < distanceToCameraSqr && distanceToCameraSqr <= ER_Utility::DistancesLOD[1] * ER_Utility::DistancesLOD[1]) {
				mCurrentLODIndex = 1;
			}
			else if (ER_Utility::DistancesLOD[1] * ER_Utility::DistancesLOD[1] < distanceToCameraSqr && distanceToCameraSqr <= ER_Utility::DistancesLOD[2] * ER_Utility::DistancesLOD[2]) {
				mCurrentLODIndex = 2;
			}
			else
				mCurrentLODIndex = -1; //culled

			mCurrentLODIndex = std::min(mCurrentLODIndex, GetLODCount());
		}
	}
}

