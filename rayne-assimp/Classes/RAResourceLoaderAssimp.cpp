//
//  RAResourceLoaderAssimp.cpp
//  rayne-assimp
//
//  Copyright 2013 by Überpixel. All rights reserved.
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//  documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
//  and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
//  INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
//  PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
//  FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
//  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#include "RAResourceLoaderAssimp.h"
#include <limits>

namespace RN
{
	namespace assimp
	{
		RNDefineMeta(AssimpResourceLoader, ResourceLoader)
			
		// ---------------------
		// MARK: -
		// MARK: AssimpResourceLoader
		// ---------------------
		
		AssimpResourceLoader::AssimpResourceLoader() :
			ResourceLoader(Model::GetMetaClass())
		{
			aiString extensionsString;
			Assimp::Importer{}.GetExtensionList(extensionsString);
			
			String *string = RNSTR(extensionsString.C_Str());
			string->ReplaceOccurrencesOfString(RNCSTR("*."), RNCSTR(""));
			
			Array *extensions = string->GetComponentsSeparatedByString(RNCSTR(";"));
			
			std::vector<std::string> myVector;
			
			extensions->Enumerate<String>([&](String *string, size_t index, bool &stop) {
				myVector.push_back(string->GetUTF8String());
			});
			
			SetFileExtensions(myVector);
		}
		
		void AssimpResourceLoader::InitialWakeUp(MetaClass *meta)
		{
			if(meta == GetMetaClass())
			{
				AssimpResourceLoader *loader = new AssimpResourceLoader();
				ResourceCoordinator::GetSharedInstance()->RegisterResourceLoader(loader);
				loader->Release();
			}
		}
		
		Asset *AssimpResourceLoader::Load(File *file, Dictionary *settings)
		{
			Model *model = new Model();
			
			bool guessMaterial = true;
			bool recalculateNormals = false;
			float smoothNormalAngle = 20.0f;
			bool autoloadLOD = false;
			size_t stage = 0;
			
			if(settings->GetObjectForKey(RNCSTR("guessMaterial")))
			{
				Number *number = settings->GetObjectForKey<Number>(RNCSTR("guessMaterial"));
				guessMaterial = number->GetBoolValue();
			}
			
			if(settings->GetObjectForKey(RNCSTR("recalculateNormals")))
			{
				Number *number = settings->GetObjectForKey<Number>(RNCSTR("recalculateNormals"));
				recalculateNormals = number->GetBoolValue();
			}
			
			if(settings->GetObjectForKey(RNCSTR("smoothNormalAngle")))
			{
				Number *number = settings->GetObjectForKey<Number>(RNCSTR("smoothNormalAngle"));
				smoothNormalAngle = number->GetFloatValue();
			}
			
			if(settings->GetObjectForKey(RNCSTR("autoloadLOD")))
			{
				Number *number = settings->GetObjectForKey<Number>(RNCSTR("autoloadLOD"));
				autoloadLOD = number->GetBoolValue();
			}
			
			Assimp::Importer importer;
			importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, smoothNormalAngle);
			
			const aiScene *scene = importer.ReadFile(file->GetFullPath(), NULL);
			if(recalculateNormals)
			{
				importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, aiComponent_NORMALS|aiComponent_TANGENTS_AND_BITANGENTS);
				scene = importer.ApplyPostProcessing(aiProcess_RemoveComponent);
			}
			scene = importer.ApplyPostProcessing(aiProcessPreset_TargetRealtime_Quality | aiProcess_OptimizeGraph | aiProcess_OptimizeMeshes | aiProcess_FlipUVs);
			
			if(!scene)
				throw Exception(Exception::Type::GenericException, importer.GetErrorString());
			
			LoadLODStage(scene, model, stage, file->GetPath(), guessMaterial);
			
			if(scene->mNumAnimations > 0)
				LoadSkeleton(scene, model);
			
			std::string base = PathManager::Basepath(file->GetFullPath());
			std::string name = PathManager::Basename(file->GetFullPath());
			std::string extension = PathManager::Extension(file->GetFullPath());
			
			std::vector<float> lodFactors = Model::GetDefaultLODFactors();
			
			while(autoloadLOD && stage < lodFactors.size())
			{
				std::stringstream stream;
				stream << name << "_lod" << (stage + 1) << "." << extension;
				
				std::string lodPath = PathManager::Join(base, stream.str());
				
				try
				{
					const aiScene *scene = importer.ReadFile(lodPath, NULL);
					if(recalculateNormals)
					{
						importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, aiComponent_NORMALS);
						scene = importer.ApplyPostProcessing(aiProcess_RemoveComponent);
					}
					scene = importer.ApplyPostProcessing(aiProcessPreset_TargetRealtime_Quality | aiProcess_OptimizeGraph | aiProcess_OptimizeMeshes | aiProcess_FlipUVs);
					
					if(!scene)
						throw Exception(Exception::Type::GenericException, importer.GetErrorString());
					
					stage = model->AddLODStage(lodFactors[stage]);
					LoadLODStage(scene, model, stage, lodPath, guessMaterial);
				}
				catch(Exception e)
				{
					break;
				}
			}
			
			return model;
		}
		
		Texture *AssimpResourceLoader::GetTexture(aiMaterial *aimaterial, const std::string &filepath, aiTextureType aitexturetype, uint8 index)
		{
			aiString aipath;
			aimaterial->GetTexture(aitexturetype, index, &aipath);
			
			std::string base = PathManager::Basename(aipath.C_Str());
			std::string extension = PathManager::Extension(aipath.C_Str());
			
			std::stringstream path;
			path << PathManager::Join(filepath, base) << "." << extension;
			
			bool linear = (aitexturetype == aiTextureType_NORMALS || aitexturetype == aiTextureType_HEIGHT || aitexturetype == aiTextureType_DISPLACEMENT);
			
			std::string normalized = FileManager::GetSharedInstance()->GetNormalizedPathFromFullpath(path.str());
			return Texture::WithFile(normalized, linear);
		}
		
		void AssimpResourceLoader::LoadLODStage(const aiScene *scene, Model *model, size_t stage, const std::string &filepath, bool guessMaterial)
		{
			Shader *shader = ResourceCoordinator::GetSharedInstance()->GetResourceWithName<Shader>(kRNResourceKeyDefaultShader, nullptr);
			
			int boneindexoffset = 0;
			
			for(int i = 0; i < scene->mNumMeshes; i++)
			{
				aiMesh *aimesh = scene->mMeshes[i];
				aiMaterial *aimaterial = scene->mMaterials[aimesh->mMaterialIndex];
				
				Material *material = new Material(shader);
				if(aimaterial->GetTextureCount(aiTextureType_DIFFUSE) > 0)
				{
					material->AddTexture(GetTexture(aimaterial, filepath, aiTextureType_DIFFUSE));
				}
				
				if(aimaterial->GetTextureCount(aiTextureType_NORMALS) > 0)
				{
					material->AddTexture(GetTexture(aimaterial, filepath, aiTextureType_NORMALS));
					material->Define("RN_NORMALMAP");
					
				}
				
				if(aimaterial->GetTextureCount(aiTextureType_SPECULAR) > 0)
				{
					material->AddTexture(GetTexture(aimaterial, filepath, aiTextureType_SPECULAR));
					material->Define("RN_SPECULARITY");
					material->Define("RN_SPECMAP");
				}
				
				std::vector<MeshDescriptor> descriptors;
				
				MeshDescriptor meshDescriptor(MeshFeature::Vertices);
				if(aimesh->HasPositions())
				{
					meshDescriptor.elementSize = sizeof(Vector3);
					meshDescriptor.elementMember = 3;
					descriptors.push_back(meshDescriptor);
				}
				
				if(aimesh->HasNormals())
				{
					meshDescriptor = MeshDescriptor(MeshFeature::Normals);
					meshDescriptor.elementSize = sizeof(Vector3);
					meshDescriptor.elementMember = 3;
					descriptors.push_back(meshDescriptor);
				}
				
				if(aimesh->HasTextureCoords(0))
				{
					meshDescriptor = MeshDescriptor(MeshFeature::UVSet0);
					meshDescriptor.elementSize = sizeof(Vector3);
					meshDescriptor.elementMember = 3;
					descriptors.push_back(meshDescriptor);
				}
				
				if(aimesh->HasTextureCoords(1))
				{
					meshDescriptor = MeshDescriptor(MeshFeature::UVSet1);
					meshDescriptor.elementSize = sizeof(Vector3);
					meshDescriptor.elementMember = 3;
					descriptors.push_back(meshDescriptor);
				}
				
				if(aimesh->HasTangentsAndBitangents())
				{
					meshDescriptor = MeshDescriptor(MeshFeature::Tangents);
					meshDescriptor.elementSize = sizeof(Vector4);
					meshDescriptor.elementMember = 4;
					descriptors.push_back(meshDescriptor);
				}
				
				float *boneWeights = nullptr;
				float *boneIndices = nullptr;
				
				if(aimesh->HasBones())
				{
					boneWeights = new float[aimesh->mNumVertices*4];
					boneIndices = new float[aimesh->mNumVertices*4];
					
					std::fill(boneWeights, &boneWeights[aimesh->mNumVertices*4], -1.0f);
					std::fill(boneIndices, &boneIndices[aimesh->mNumVertices*4], -1.0f);
					
					for(int ind = 0; ind < aimesh->mNumBones; ind++)
					{
						aiBone *aibone = aimesh->mBones[ind];
						for(int w = 0; w < aibone->mNumWeights; w++)
						{
							aiVertexWeight *aiweight = &aibone->mWeights[w];
							for(int n = 0; n < 4; n++)
							{
								if(boneWeights[aiweight->mVertexId*4+n] < -0.5f)
								{
									boneWeights[aiweight->mVertexId*4+n] = aiweight->mWeight;
									boneIndices[aiweight->mVertexId*4+n] = ind+boneindexoffset;
									boneIndices[aiweight->mVertexId*4+n] += 0.1f;
									break;
								}
							}
						}
					}
					
					for(int ind = 0; ind < aimesh->mNumVertices; ind++)
					{
						for(int n = 0; n < 4; n++)
						{
							if(boneWeights[ind*4+n] < -0.5f)
							{
								boneWeights[ind*4+n] = 0.0f;
								boneIndices[ind*4+n] = 0.0f;
							}
						}
					}
					
					boneindexoffset += aimesh->mNumBones;
					
					meshDescriptor = MeshDescriptor(MeshFeature::BoneIndices);
					meshDescriptor.elementSize = sizeof(Vector4);
					meshDescriptor.elementMember = 4;
					descriptors.push_back(meshDescriptor);
					
					meshDescriptor = MeshDescriptor(MeshFeature::BoneWeights);
					meshDescriptor.elementSize = sizeof(Vector4);
					meshDescriptor.elementMember = 4;
					descriptors.push_back(meshDescriptor);
				}
				
				uint32 indexCount = 0;
				uint8 *indices = nullptr;
				
				if(aimesh->HasFaces())
				{
					uint8 indicesSize = 2;
					if(aimesh->mNumFaces*3 > 65535)
					{
						indicesSize = 4;
					}
					
					indices = new uint8[indicesSize*aimesh->mNumFaces*3];
					for(int face = 0; face < aimesh->mNumFaces; face++)
					{
						if(aimesh->mFaces[face].mNumIndices != 3)
						{
							continue;
						}
						for(int ind = 0; ind < aimesh->mFaces[face].mNumIndices; ind++)
						{
							if(indicesSize == 2)
							{
								((uint16*)indices)[indexCount] = static_cast<uint16>(aimesh->mFaces[face].mIndices[ind]);
							}
							if(indicesSize == 4)
							{
								((uint32*)indices)[indexCount] = static_cast<uint32>(aimesh->mFaces[face].mIndices[ind]);
							}
							indexCount++;
						}
					}
					
					meshDescriptor = MeshDescriptor(MeshFeature::Indices);
					meshDescriptor.elementSize = indicesSize;
					meshDescriptor.elementMember = 1;
					descriptors.push_back(meshDescriptor);
				}
				
				Mesh *mesh = new Mesh(descriptors, aimesh->mNumVertices, indexCount);
				Mesh::Chunk chunk = mesh->GetChunk();
				
				if(aimesh->HasPositions())
				{
					chunk.SetData(aimesh->mVertices, MeshFeature::Vertices);
				}
				
				if(aimesh->HasTextureCoords(0))
				{
					chunk.SetData(aimesh->mTextureCoords[0], MeshFeature::UVSet0);
				}
				
				if(aimesh->HasTextureCoords(1))
				{
					chunk.SetData(aimesh->mTextureCoords[1], MeshFeature::UVSet1);
				}
				
				if(aimesh->HasTangentsAndBitangents())
				{
					Mesh::ElementIterator<Vector4> it = chunk.GetIterator<Vector4>(MeshFeature::Tangents);
					
					for(int ind = 0; ind < aimesh->mNumVertices; ind++)
					{
						Vector4 tangent(aimesh->mTangents[ind].x, aimesh->mTangents[ind].y, aimesh->mTangents[ind].z, 0.0f);
						Vector3 normal(aimesh->mNormals[ind].x, aimesh->mNormals[ind].y, aimesh->mNormals[ind].z);
						Vector3 binormal(aimesh->mBitangents[ind].x, aimesh->mBitangents[ind].y, aimesh->mBitangents[ind].z);
						Vector3 inversebinormal = normal.GetCrossProduct(Vector3(tangent));
						
						if(binormal.GetDotProduct(inversebinormal) > 0.0f)
						{
							tangent.w = 1.0f;
						}
						else
						{
							tangent.w = -1.0f;
						}
						
						if(isnan(normal.x) || isnan(normal.y) || isnan(normal.z))
						{
							normal = RN::Vector3(0.0f, -1.0f, 0.0f);
							aimesh->mNormals[ind].x = 0.0f;
							aimesh->mNormals[ind].y = -1.0f;
							aimesh->mNormals[ind].z = 0.0f;
						}
						
						if(isnan(tangent.x) || isnan(tangent.y) || isnan(tangent.z))
						{
							Vector3 newnormal(normal);
							newnormal.x += 1.0f;
							tangent = Vector4(newnormal.GetCrossProduct(normal).Normalize(), 1.0f);
						}
						
						it->x = tangent.x;
						it->y = tangent.y;
						it->z = tangent.z;
						it->w = tangent.w;
						it++;
					}
				}
				
				if(aimesh->HasNormals())
				{
					chunk.SetData(aimesh->mNormals, MeshFeature::Normals);
				}
				
				if(aimesh->HasBones())
				{
					chunk.SetData(boneWeights, MeshFeature::BoneWeights);
					chunk.SetData(boneIndices, MeshFeature::BoneIndices);
				}
				
				chunk.CommitChanges();
				
				if(aimesh->HasFaces())
				{
					chunk = mesh->GetIndicesChunk();
					chunk.SetData(indices, MeshFeature::Indices);
					delete[] indices;
					chunk.CommitChanges();
				}
				
				mesh->CalculateBoundingVolumes();
				model->AddMesh(mesh, material, stage);
			}
		}
		
		void AssimpResourceLoader::WalkForgottenBones(aiNode *ainode, std::vector<aiNode*> &ainodes)
		{
			if(std::find(ainodes.begin(), ainodes.end(), ainode) == ainodes.end())
			{
				ainodes.push_back(ainode);
				if(ainode->mParent)
					WalkForgottenBones(ainode->mParent, ainodes);
			}
		}
		
		void AssimpResourceLoader::CopyMatrix(aiMatrix4x4 &from, Matrix &to)
		{
			to.m[0] = from.a1;
			to.m[4] = from.a2;
			to.m[8] = from.a3;
			to.m[12] = from.a4;
			
			to.m[1] = from.b1;
			to.m[5] = from.b2;
			to.m[9] = from.b3;
			to.m[13] = from.b4;
			
			to.m[2] = from.c1;
			to.m[6] = from.c2;
			to.m[10] = from.c3;
			to.m[14] = from.c4;
			
			to.m[3] = from.d1;
			to.m[7] = from.d2;
			to.m[11] = from.d3;
			to.m[15] = from.d4;
		}
		
		void AssimpResourceLoader::CopyMatrix(Matrix &from, aiMatrix4x4 &to)
		{
			to.a1 = from.m[0];
			to.a2 = from.m[4];
			to.a3 = from.m[8];
			to.a4 = from.m[12];
			
			to.b1 = from.m[1];
			to.b1 = from.m[5];
			to.b1 = from.m[9];
			to.b1 = from.m[13];
			
			to.c1 = from.m[2];
			to.c1 = from.m[6];
			to.c1 = from.m[10];
			to.c1 = from.m[14];
			
			to.d1 = from.m[3];
			to.d1 = from.m[7];
			to.d1 = from.m[11];
			to.d1 = from.m[15];
		}
		
		void AssimpResourceLoader::LoadSkeleton(const aiScene* scene, RN::Model *model)
		{
			Skeleton *skeleton = new Skeleton();
			
			//Create list of valid bones
			std::vector<aiNode*> aibonenodes;
			for(int i = 0; i < scene->mNumMeshes; i++)
			{
				aiMesh *aimesh = scene->mMeshes[i];
				for(int b = 0; b < aimesh->mNumBones; b++)
				{
					aiBone *aibone = aimesh->mBones[b];
					aiNode *ainode = scene->mRootNode->FindNode(aibone->mName);
					aibonenodes.push_back(ainode);
				}
			}
			
			size_t numusednodes = aibonenodes.size();
			
			//Find unattached bones and add them to the end of the list
			for(int i = 0; i < aibonenodes.size(); i++)
			{
				aiNode *ainode = aibonenodes[i];
				if(ainode->mParent && std::find(aibonenodes.begin(), aibonenodes.end(), ainode->mParent) == aibonenodes.end())
				{
					WalkForgottenBones(ainode->mParent, aibonenodes);
				}
			}
			
			//list of nodes that are already used as children
			std::vector<size_t> ainodechildren;
			
			//create valid bones, determine if they are root bones and add the valid children
			for(int i = 0; i < scene->mNumMeshes; i++)
			{
				aiMesh *aimesh = scene->mMeshes[i];
				for(int b = 0; b < aimesh->mNumBones; b++)
				{
					aiBone *aibone = aimesh->mBones[b];
					aiNode *ainode = scene->mRootNode->FindNode(aibone->mName);
					
					Matrix basemat;
					CopyMatrix(aibone->mOffsetMatrix, basemat);
					Bone bone(basemat, std::string(aibone->mName.C_Str()), !ainode->mParent, true);
					
					if(ainode)
					{
						for(int c = 0; c < ainode->mNumChildren; c++)
						{
							auto aichild = std::find(aibonenodes.begin(), aibonenodes.end(), ainode->mChildren[c]);
							while(aichild != aibonenodes.end())
							{
								size_t index = std::distance(aibonenodes.begin(), aichild);
								if(std::find(ainodechildren.begin(), ainodechildren.end(), index) == ainodechildren.end())
								{
									bone.tempChildren.push_back(index);
									ainodechildren.push_back(index);
								}
								aichild = std::find(++aichild, aibonenodes.end(), ainode->mChildren[c]);
							}
						}
					}
					
					skeleton->bones.push_back(bone);
				}
			}
			
			//Add additional bones to the skeleton
			for(auto it = aibonenodes.begin()+numusednodes; it != aibonenodes.end(); it++)
			{
				aiMatrix4x4 aiOffsetMatrix = (*it)->mTransformation;
				for(aiNode *aiparent = (*it)->mParent; aiparent; aiparent = aiparent->mParent)
				{
					aiOffsetMatrix = aiparent->mTransformation*aiOffsetMatrix;
				}
				
				Matrix basemat;
				CopyMatrix(aiOffsetMatrix, basemat);
				Bone bone(basemat.GetInverse(), std::string((*it)->mName.C_Str()), !(*it)->mParent, true);
				
				if(*it)
				{
					for(int c = 0; c < (*it)->mNumChildren; c++)
					{
						auto aichild = std::find(aibonenodes.begin(), aibonenodes.end(), (*it)->mChildren[c]);
						while(aichild != aibonenodes.end())
						{
							size_t index = std::distance(aibonenodes.begin(), aichild);
							if(std::find(ainodechildren.begin(), ainodechildren.end(), index) == ainodechildren.end())
							{
								bone.tempChildren.push_back(index);
								ainodechildren.push_back(index);
							}
							aichild = std::find(++aichild, aibonenodes.end(), (*it)->mChildren[c]);
						}
					}
				}
				
				skeleton->bones.push_back(bone);
			}
			
			//Initialize skeleton
			skeleton->Init();
			
			//Create local skinning matrices
			/*std::map<size_t, Matrix> localskinningmatrices;
			size_t boneindex = 0;
			for(auto bone : skeleton->bones)
			{
				localskinningmatrices.insert(std::pair<size_t, Matrix>(boneindex++, bone.relBaseMatrix.GetInverse()));
			}*/
			
			for(int i = 0; i < scene->mNumAnimations; i++)
			{
				aiAnimation *aianimation = scene->mAnimations[i];
				if(Math::Compare(aianimation->mDuration, 0.0))
					continue;
				
				std::string animname(aianimation->mName.C_Str());
				Animation *anim = new Animation(animname);
				anim->Autorelease();
				anim->Retain();
				skeleton->animations.insert(std::pair<std::string, Animation*>(animname, anim));
				
				for(int n = 0; n < aianimation->mNumChannels; n++)
				{
					aiNodeAnim *ainodeanim = aianimation->mChannels[n];
					
					float currtime = std::numeric_limits<float>::max();
					float starttime = 0.0f;
					size_t currPosKey = 0;
					size_t currRotKey = 0;
					size_t currScalKey = 0;
					
					//Matrix aiinvskinningmatrix = localskinningmatrices[std::distance(aibonenodes.begin(), std::find(aibonenodes.begin(), aibonenodes.end(), scene->mRootNode->FindNode(ainodeanim->mNodeName)))];
					
					//find first frame time
					if(ainodeanim->mNumPositionKeys > 0)
						currtime = fminf(ainodeanim->mPositionKeys[0].mTime, currtime);
					if(ainodeanim->mNumRotationKeys > 0)
						currtime = fminf(ainodeanim->mRotationKeys[0].mTime, currtime);
					if(ainodeanim->mNumScalingKeys > 0)
						currtime = fminf(ainodeanim->mScalingKeys[0].mTime, currtime);
					
					starttime = currtime;
					
					AnimationBone *animbone = 0;
					while(1)
					{
						aiVector3D aipos = ainodeanim->mPositionKeys[currPosKey].mValue;
						Vector3 animbonepos(aipos.x, aipos.y, aipos.z);
						aiVector3D aiscal = ainodeanim->mScalingKeys[currScalKey].mValue;
						Vector3 animbonescale(aiscal.x, aiscal.y, aiscal.z);
						aiQuaternion airot = ainodeanim->mRotationKeys[currRotKey].mValue;
						Quaternion animbonerot(airot.x, airot.y, airot.z, airot.w);
						
						
						//Do blending for other key frames
						if(ainodeanim->mNumPositionKeys > currPosKey+1)
						{
							aiVector3D ainextpos = ainodeanim->mPositionKeys[currPosKey+1].mValue;
							float currframetime = ainodeanim->mPositionKeys[currPosKey].mTime;
							float nextframetime = ainodeanim->mPositionKeys[currPosKey+1].mTime;
							float factor = (currtime-currframetime)/(nextframetime-currframetime);
							animbonepos = animbonepos.GetLerp(Vector3(ainextpos.x, ainextpos.y, ainextpos.z), factor);
						}
						if(ainodeanim->mNumScalingKeys > currScalKey+1)
						{
							aiVector3D ainextscal = ainodeanim->mScalingKeys[currScalKey+1].mValue;
							float currframetime = ainodeanim->mScalingKeys[currScalKey].mTime;
							float nextframetime = ainodeanim->mScalingKeys[currScalKey+1].mTime;
							float factor = (currtime-currframetime)/(nextframetime-currframetime);
							animbonescale = animbonescale.GetLerp(Vector3(ainextscal.x, ainextscal.y, ainextscal.z), factor);
						}
						if(ainodeanim->mNumRotationKeys > currRotKey+1)
						{
							aiQuaternion ainextrot = ainodeanim->mRotationKeys[currRotKey+1].mValue;
							float currframetime = ainodeanim->mRotationKeys[currRotKey].mTime;
							float nextframetime = ainodeanim->mRotationKeys[currRotKey+1].mTime;
							float factor = (currtime-currframetime)/(nextframetime-currframetime);
							animbonerot = animbonerot.GetLerpSpherical(Quaternion(ainextrot.x, ainextrot.y, ainextrot.z, ainextrot.w), factor);
						}
						
						//Create keyframe
						animbone = new AnimationBone(animbone, 0, currtime-starttime, animbonepos, animbonescale, animbonerot);
						
						if(ainodeanim->mNumPositionKeys == currPosKey+1 && ainodeanim->mNumRotationKeys == currRotKey+1 && ainodeanim->mNumScalingKeys == currScalKey+1)
							break;
						
						currtime = std::numeric_limits<float>::max();
						if(ainodeanim->mNumPositionKeys > currPosKey+1)
							currtime = fminf(ainodeanim->mPositionKeys[currPosKey+1].mTime, currtime);
						if(ainodeanim->mNumRotationKeys > currRotKey+1)
							currtime = fminf(ainodeanim->mRotationKeys[currRotKey+1].mTime, currtime);
						if(ainodeanim->mNumScalingKeys > currScalKey+1)
							currtime = fminf(ainodeanim->mScalingKeys[currScalKey+1].mTime, currtime);
						
						if(ainodeanim->mNumPositionKeys > currPosKey+1)
							if(Math::Compare(currtime, static_cast<float>(ainodeanim->mPositionKeys[currPosKey+1].mTime)))
								currPosKey += 1;
						if(ainodeanim->mNumRotationKeys > currRotKey+1)
							if(Math::Compare(currtime, static_cast<float>(ainodeanim->mRotationKeys[currRotKey+1].mTime)))
								currRotKey += 1;
						if(ainodeanim->mNumScalingKeys > currScalKey+1)
							if(Math::Compare(currtime, static_cast<float>(ainodeanim->mScalingKeys[currScalKey+1].mTime)))
								currScalKey += 1;
					}
					
					AnimationBone *lastbone = animbone;
					while(animbone->prevFrame != 0)
					{
						animbone->prevFrame->nextFrame = animbone;
						animbone = animbone->prevFrame;
					}
					animbone->prevFrame = lastbone;
					lastbone->nextFrame = animbone;
					
					auto aichild = std::find(aibonenodes.begin(), aibonenodes.end(), scene->mRootNode->FindNode(ainodeanim->mNodeName));
					while(aichild != aibonenodes.end())
					{
						size_t boneid = std::distance(aibonenodes.begin(), aichild);
						anim->bones.insert(std::pair<size_t, AnimationBone*>(boneid, animbone));
						aichild = std::find(++aichild, aibonenodes.end(), scene->mRootNode->FindNode(ainodeanim->mNodeName));
					}
				}
			}
			
			for(auto anim : skeleton->animations)
			{
				if(anim.second->GetLength() <= k::EpsilonFloat)
				{
					for(auto bone : anim.second->bones)
					{
						float time = 0.0f;
						AnimationBone *first = bone.second;
						AnimationBone *temp = bone.second;
						while(temp != nullptr && temp != first)
						{
							temp->time = time;
							time += 1.0f;
							temp = temp->nextFrame;
						}
					}
				}
			}
			
			model->SetSkeleton(skeleton);
		}
		
		bool AssimpResourceLoader::SupportsLoadingFile(File *file)
		{
			return true;
		}
		
		bool AssimpResourceLoader::SupportsBackgroundLoading()
		{
			return true;
		}
		
		uint32 AssimpResourceLoader::GetPriority() const
		{
			return kRNResourceCoordinatorBuiltInPriority;
		}
	}
}
