/*
nifopt: convert and optimize between NiTriShape and BSTriShape.
Copyright (C) 2018  Parco Opaai

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "NifFile.h"

// OptimizeForSSE function from
// https://github.com/ousnius/BodySlide-and-Outfit-Studio/blob/dev/lib/NIF/NifFile.cpp

struct OptOptions {
#if 0
	NiVersion targetVersion;
#endif
	bool headParts = false;
	bool removeParallax = true;
	bool calcBounds = true;
};

struct OptResult {
	bool versionMismatch = false;
	bool dupesRenamed = false;
	std::vector<std::string> shapesVColorsRemoved;
	std::vector<std::string> shapesNormalsRemoved;
	std::vector<std::string> shapesPartTriangulated;
	std::vector<std::string> shapesTangentsAdded;
	std::vector<std::string> shapesParallaxRemoved;
};

OptResult OptimizeForSSE(NifFile& nif, const OptOptions& options = OptOptions())
{
	OptResult result;

	NiHeader& hdr = nif.GetHeader();
	NiVersion& version = hdr.GetVersion();

	if (!(version.User() == 12 && version.Stream() == 83)) {
		result.versionMismatch = true;
		return result;
	}

	if (!nif.IsTerrain())
		result.dupesRenamed = nif.RenameDuplicateShapes();

	version.SetFile(V20_2_0_7);
	version.SetUser(12);
	version.SetStream(100);

	for (auto &shape : nif.GetShapes()) {
		std::string shapeName = shape->GetName();

		auto geomData = hdr.GetBlock<NiGeometryData>(shape->GetDataRef());
		if (geomData) {
			bool removeVertexColors = true;
			bool hasTangents = geomData->HasTangents();
			std::vector<Vector3>* vertices = &geomData->vertices;
			std::vector<Vector3>* normals = &geomData->normals;
			const std::vector<Color4>& colors = geomData->vertexColors;
			std::vector<Vector2>* uvs = nullptr;
			if (!geomData->uvSets.empty())
				uvs = &geomData->uvSets[0];

			std::vector<Triangle> triangles;
			geomData->GetTriangles(triangles);

			// Only remove vertex colors if all are 0xFFFFFFFF
			Color4 white(1.0f, 1.0f, 1.0f, 1.0f);
			for (auto &c : colors) {
				if (white != c) {
					removeVertexColors = false;
					break;
				}
			}

			bool headPartEyes = false;
			NiShader* shader = nif.GetShader(shape);
			if (shader) {
				auto bslsp = dynamic_cast<BSLightingShaderProperty*>(shader);
				if (bslsp) {
					// Remember eyes flag for later
					if ((bslsp->shaderFlags1 & (1 << 17)) != 0)
						headPartEyes = true;

					// No normals and tangents with model space maps
					if (bslsp->IsModelSpace()) {
						if (!normals->empty())
							result.shapesNormalsRemoved.push_back(shapeName);

						normals = nullptr;
					}

					// Check tree anim flag
					if ((bslsp->shaderFlags2 & (1 << 29)) != 0)
						removeVertexColors = false;

					// Disable flags if vertex colors were removed
					if (removeVertexColors) {
						bslsp->SetVertexColors(false);
						bslsp->SetVertexAlpha(false);
					}

					if (options.removeParallax) {
						if (bslsp->GetShaderType() == BSLSP_PARALLAX) {
							// Change type from parallax to default
							bslsp->SetShaderType(BSLSP_DEFAULT);

							// Remove parallax flag
							bslsp->shaderFlags1 &= ~(1 << 11);

							// Remove parallax texture from set
							auto textureSet = hdr.GetBlock<BSShaderTextureSet>(shader->GetTextureSetRef());
							if (textureSet && textureSet->numTextures >= 4)
								textureSet->textures[3].Clear();

							result.shapesParallaxRemoved.push_back(shapeName);
						}
					}
				}

				auto bsesp = dynamic_cast<BSEffectShaderProperty*>(shader);
				if (bsesp) {
					// Remember eyes flag for later
					if ((bsesp->shaderFlags1 & (1 << 17)) != 0)
						headPartEyes = true;

					// Check tree anim flag
					if ((bsesp->shaderFlags2 & (1 << 29)) != 0)
						removeVertexColors = false;

					// Disable flags if vertex colors were removed
					if (removeVertexColors) {
						bsesp->SetVertexColors(false);
						bsesp->SetVertexAlpha(false);
					}
				}
			}

			if (!colors.empty() && removeVertexColors)
				result.shapesVColorsRemoved.push_back(shapeName);

			BSTriShape* bsOptShape = nullptr;
			auto bsSegmentShape = dynamic_cast<BSSegmentedTriShape*>(shape);
			if (bsSegmentShape) {
				bsOptShape = new BSSubIndexTriShape();
			}
			else {
				if (options.headParts)
					bsOptShape = new BSDynamicTriShape();
				else
					bsOptShape = new BSTriShape();
			}

			bsOptShape->SetName(shape->GetName());
			bsOptShape->SetControllerRef(shape->GetControllerRef());
			bsOptShape->SetSkinInstanceRef(shape->GetSkinInstanceRef());
			bsOptShape->SetShaderPropertyRef(shape->GetShaderPropertyRef());
			bsOptShape->SetAlphaPropertyRef(shape->GetAlphaPropertyRef());
			bsOptShape->SetCollisionRef(shape->GetCollisionRef());
			bsOptShape->GetProperties() = shape->GetProperties();
			bsOptShape->GetExtraData() = shape->GetExtraData();

			bsOptShape->transform = shape->transform;

			bsOptShape->Create(vertices, &triangles, uvs, normals);
			bsOptShape->flags = shape->flags;

			// Move segments to new shape
			if (bsSegmentShape) {
				auto bsSITS = dynamic_cast<BSSubIndexTriShape*>(bsOptShape);
				bsSITS->numSegments = bsSegmentShape->numSegments;
				bsSITS->segments = bsSegmentShape->segments;
			}

			// Restore old bounds for static meshes or when calc bounds is off
			if (!shape->IsSkinned() || !options.calcBounds)
				bsOptShape->SetBounds(geomData->GetBounds());

			// Vertex Colors
			if (bsOptShape->GetNumVertices() > 0) {
				if (!removeVertexColors && colors.size() > 0) {
					bsOptShape->SetVertexColors(true);
					for (int i = 0; i < bsOptShape->GetNumVertices(); i++) {
						auto& vertex = bsOptShape->vertData[i];

						float f = std::max(0.0f, std::min(1.0f, colors[i].r));
						vertex.colorData[0] = (byte)std::floor(f == 1.0f ? 255 : f * 256.0);

						f = std::max(0.0f, std::min(1.0f, colors[i].g));
						vertex.colorData[1] = (byte)std::floor(f == 1.0f ? 255 : f * 256.0);

						f = std::max(0.0f, std::min(1.0f, colors[i].b));
						vertex.colorData[2] = (byte)std::floor(f == 1.0f ? 255 : f * 256.0);

						f = std::max(0.0f, std::min(1.0f, colors[i].a));
						vertex.colorData[3] = (byte)std::floor(f == 1.0f ? 255 : f * 256.0);
					}
				}

				// Find NiOptimizeKeep string
				for (auto& extraData : bsOptShape->GetExtraData()) {
					auto stringData = hdr.GetBlock<NiStringExtraData>(extraData.GetIndex());
					if (stringData) {
						if (stringData->GetStringData().find("NiOptimizeKeep") != std::string::npos) {
							bsOptShape->particleDataSize = bsOptShape->GetNumVertices() * 6 + triangles.size() * 3;
							bsOptShape->particleVerts = *vertices;

							bsOptShape->particleNorms.resize(vertices->size(), Vector3(1.0f, 0.0f, 0.0f));
							if (normals && normals->size() == vertices->size())
								bsOptShape->particleNorms = *normals;

							bsOptShape->particleTris = triangles;
						}
					}
				}

				// Skinning and partitions
				if (shape->IsSkinned()) {
					bsOptShape->SetSkinned(true);

					auto skinInst = hdr.GetBlock<NiSkinInstance>(shape->GetSkinInstanceRef());
					if (skinInst) {
						auto skinPart = hdr.GetBlock<NiSkinPartition>(skinInst->GetSkinPartitionRef());
						if (skinPart) {
							bool triangulated = nif.TriangulatePartitions(shape);
							if (triangulated)
								result.shapesPartTriangulated.push_back(shapeName);

							for (int partID = 0; partID < skinPart->numPartitions; partID++) {
								NiSkinPartition::PartitionBlock& part = skinPart->partitions[partID];

								for (int i = 0; i < part.numVertices; i++) {
									const ushort v = part.vertexMap[i];

									if (bsOptShape->vertData.size() > v) {
										auto& vertex = bsOptShape->vertData[v];

										if (part.hasVertexWeights) {
											auto& weights = part.vertexWeights[i];
											vertex.weights[0] = weights.w1;
											vertex.weights[1] = weights.w2;
											vertex.weights[2] = weights.w3;
											vertex.weights[3] = weights.w4;
										}

										if (part.hasBoneIndices) {
											auto& boneIndices = part.boneIndices[i];
											vertex.weightBones[0] = part.bones[boneIndices.i1];
											vertex.weightBones[1] = part.bones[boneIndices.i2];
											vertex.weightBones[2] = part.bones[boneIndices.i3];
											vertex.weightBones[3] = part.bones[boneIndices.i4];
										}
									}
								}

								std::vector<Triangle> realTris(part.numTriangles);
								for (int i = 0; i < part.numTriangles; i++) {
									auto& partTri = part.triangles[i];

									// Find the actual tri index from the partition tri index
									Triangle tri;
									tri.p1 = part.vertexMap[partTri.p1];
									tri.p2 = part.vertexMap[partTri.p2];
									tri.p3 = part.vertexMap[partTri.p3];

									tri.rot();
									realTris[i] = tri;
								}

								part.triangles = realTris;
								part.trueTriangles = realTris;
							}
						}
					}
				}
				else
					bsOptShape->SetSkinned(false);
			}
			else
				bsOptShape->SetVertices(false);

			// Check if tangents were added
			if (!hasTangents && bsOptShape->HasTangents())
				result.shapesTangentsAdded.push_back(shapeName);

			// Enable eye data flag
			if (!bsSegmentShape) {
				if (options.headParts) {
					if (headPartEyes)
						bsOptShape->SetEyeData(true);
				}
			}

			hdr.ReplaceBlock(nif.GetBlockID(shape), bsOptShape);
			nif.UpdateSkinPartitions(bsOptShape);
		}
	}

	nif.DeleteUnreferencedBlocks();

	// For files without a root node, remove the leftover data blocks anyway
	hdr.DeleteBlockByType("NiTriStripsData", true);
	hdr.DeleteBlockByType("NiTriShapeData", true);

	return result;
}

void RevertForSLE(NifFile& nif, const OptOptions& options = OptOptions())
{
	OptResult result;

	NiHeader& hdr = nif.GetHeader();
	NiVersion& version = hdr.GetVersion();

	if (!nif.IsTerrain())
		result.dupesRenamed = nif.RenameDuplicateShapes();

	version.SetStream(83);

	for (auto &shape : nif.GetShapes()) {
		std::string shapeName = shape->GetName();

		auto bsTriShape = dynamic_cast<BSTriShape*>(shape);
		if (bsTriShape) {
			bool removeVertexColors = true;
			bool hasTangents = bsTriShape->HasTangents();
			std::vector<Vector3>* vertices = bsTriShape->GetRawVerts();
			std::vector<Vector3>* normals = bsTriShape->GetNormalData(false);
			std::vector<Color4>* colors = bsTriShape->GetColorData();
			std::vector<Vector2>* uvs = bsTriShape->GetUVData();

			std::vector<Triangle> triangles;
			bsTriShape->GetTriangles(triangles);

			// Only remove vertex colors if all are 0xFFFFFFFF
			if (colors) {
				Color4 white(1.0f, 1.0f, 1.0f, 1.0f);
				for (auto &c : (*colors)) {
					if (white != c) {
						removeVertexColors = false;
						break;
					}
				}
			}

			bool headPartEyes = false;
			NiShader* shader = nif.GetShader(shape);
			if (shader) {
				auto bslsp = dynamic_cast<BSLightingShaderProperty*>(shader);
				if (bslsp) {
					// Remember eyes flag for later
					if ((bslsp->shaderFlags1 & (1 << 17)) != 0)
						headPartEyes = true;

					// No normals and tangents with model space maps
					if (bslsp->IsModelSpace()) {
						if (normals && !normals->empty())
							result.shapesNormalsRemoved.push_back(shapeName);

						normals = nullptr;
					}

					// Check tree anim flag
					if ((bslsp->shaderFlags2 & (1 << 29)) != 0)
						removeVertexColors = false;

					// Disable flags if vertex colors were removed
					if (removeVertexColors) {
						bslsp->SetVertexColors(false);
						bslsp->SetVertexAlpha(false);
					}

					if (options.removeParallax) {
						if (bslsp->GetShaderType() == BSLSP_PARALLAX) {
							// Change type from parallax to default
							bslsp->SetShaderType(BSLSP_DEFAULT);

							// Remove parallax flag
							bslsp->shaderFlags1 &= ~(1 << 11);

							// Remove parallax texture from set
							auto textureSet = hdr.GetBlock<BSShaderTextureSet>(shader->GetTextureSetRef());
							if (textureSet && textureSet->numTextures >= 4)
								textureSet->textures[3].Clear();

							result.shapesParallaxRemoved.push_back(shapeName);
						}
					}
				}

				auto bsesp = dynamic_cast<BSEffectShaderProperty*>(shader);
				if (bsesp) {
					// Remember eyes flag for later
					if ((bsesp->shaderFlags1 & (1 << 17)) != 0)
						headPartEyes = true;

					// Check tree anim flag
					if ((bsesp->shaderFlags2 & (1 << 29)) != 0)
						removeVertexColors = false;

					// Disable flags if vertex colors were removed
					if (removeVertexColors) {
						bsesp->SetVertexColors(false);
						bsesp->SetVertexAlpha(false);
					}
				}
			}

			if (colors && !colors->empty() && removeVertexColors)
				result.shapesVColorsRemoved.push_back(shapeName);

			NiTriShape* bsOptShape = nullptr;
			auto bsOptShapeData = new NiTriShapeData();
			auto bsSITS = dynamic_cast<BSSubIndexTriShape*>(shape);
			if (bsSITS)
				bsOptShape = new BSSegmentedTriShape();
			else
				bsOptShape = new NiTriShape();

			int dataId = hdr.AddBlock(bsOptShapeData);
			bsOptShape->SetDataRef(dataId);
			bsOptShape->SetGeomData(bsOptShapeData);
			bsOptShapeData->Create(vertices, &triangles, uvs, normals);

			bsOptShape->SetName(shape->GetName());
			bsOptShape->SetControllerRef(shape->GetControllerRef());
			bsOptShape->SetSkinInstanceRef(shape->GetSkinInstanceRef());
			bsOptShape->SetShaderPropertyRef(shape->GetShaderPropertyRef());
			bsOptShape->SetAlphaPropertyRef(shape->GetAlphaPropertyRef());
			bsOptShape->SetCollisionRef(shape->GetCollisionRef());
			bsOptShape->GetProperties() = shape->GetProperties();
			bsOptShape->GetExtraData() = shape->GetExtraData();

			bsOptShape->transform = shape->transform;
			bsOptShape->flags = shape->flags;

			// Move segments to new shape
			if (bsSITS) {
				auto bsSegmentShape = dynamic_cast<BSSegmentedTriShape*>(bsOptShape);
				bsSegmentShape->numSegments = bsSITS->numSegments;
				bsSegmentShape->segments = bsSITS->segments;
			}

			// Restore old bounds for static meshes or when calc bounds is off
			if (!shape->IsSkinned() || !options.calcBounds)
				bsOptShape->SetBounds(bsTriShape->GetBounds());

			// Vertex Colors
			if (bsOptShape->GetNumVertices() > 0) {
				if (!removeVertexColors && colors && colors->size() > 0) {
					bsOptShape->SetVertexColors(true);
					for (int i = 0; i < bsOptShape->GetNumVertices(); i++)
						bsOptShapeData->vertexColors[i] = (*colors)[i];
				}

				// Skinning and partitions
				if (shape->IsSkinned()) {
					auto skinInst = hdr.GetBlock<NiSkinInstance>(shape->GetSkinInstanceRef());
					if (skinInst) {
						auto skinPart = hdr.GetBlock<NiSkinPartition>(skinInst->GetSkinPartitionRef());
						if (skinPart) {
							bool triangulated = nif.TriangulatePartitions(shape);
							if (triangulated)
								result.shapesPartTriangulated.push_back(shapeName);

							for (int partID = 0; partID < skinPart->numPartitions; partID++) {
								NiSkinPartition::PartitionBlock& part = skinPart->partitions[partID];

								std::vector<Triangle> realTris(part.numTriangles);
								for (int i = 0; i < part.numTriangles; i++) {
									auto& partTri = part.triangles[i];
									Triangle tri;

									// Find the vertex map index of the triangle points
									auto mapIt1 = std::find(part.vertexMap.begin(), part.vertexMap.end(), partTri.p1);
									if (mapIt1 != part.vertexMap.end())
										tri.p1 = std::distance(part.vertexMap.begin(), mapIt1);

									auto mapIt2 = std::find(part.vertexMap.begin(), part.vertexMap.end(), partTri.p2);
									if (mapIt2 != part.vertexMap.end())
										tri.p2 = std::distance(part.vertexMap.begin(), mapIt2);

									auto mapIt3 = std::find(part.vertexMap.begin(), part.vertexMap.end(), partTri.p3);
									if (mapIt3 != part.vertexMap.end())
										tri.p3 = std::distance(part.vertexMap.begin(), mapIt3);

									tri.rot();
									realTris[i] = tri;
								}

								part.triangles = realTris;
								part.trueTriangles.clear();
							}
						}
					}
				}
			}
			else
				bsOptShape->SetVertices(false);

			// Check if tangents were added
			if (!hasTangents && bsOptShape->HasTangents())
				result.shapesTangentsAdded.push_back(shapeName);

			hdr.ReplaceBlock(nif.GetBlockID(shape), bsOptShape);
			nif.UpdateSkinPartitions(bsOptShape);
		}
	}

	nif.DeleteUnreferencedBlocks();
	nif.PrettySortBlocks();
}

#include <string>

void ChangeExtension(std::string& filename, const std::string& extension)
{
	std::string::size_type i = filename.rfind('.', filename.length());

	if (i != std::string::npos) {
		filename.replace(i, extension.length(), extension);
	}
}

int main(int argc, char* const argv[], char* const envp[])
{
	if (argc != 2) {
		std::cout << "Usage: nifopt.exe your.nif" << std::endl;
		return 1;
	}

	NifFile nif;

	char* filename = argv[1];
	nif.Load(filename);

	NiHeader& hdr = nif.GetHeader();
	NiVersion& version = hdr.GetVersion();
	printf("NiVersion file: 0x%08x user: %u stream: %u\n", version.File(), version.User(), version.Stream());

	if (version.File() != 0x14020007) {
		printf("not supported.\n");
		return 0;
	}

	if (version.User() == 12 && version.Stream() == 83) {
		printf("Optimize for Skyrim SE.\n");
		OptimizeForSSE(nif);

		std::string opt_filename = std::string(filename);
		ChangeExtension(opt_filename, ".opt.nif");

		printf("Save to %s\n", opt_filename.c_str());
		nif.Save(opt_filename.c_str());
	}
	else if (version.User() == 12 && version.Stream() == 100) {
		printf("Revert for Skyrim LE.\n");
		RevertForSLE(nif);

		std::string rev_filename = std::string(filename);
		ChangeExtension(rev_filename, ".rev.nif");

		printf("Save to %s\n", rev_filename.c_str());
		nif.Save(rev_filename.c_str());
	}
	else {
		printf("not supported.\n");
		return 0;
	}

	return 0;
}
