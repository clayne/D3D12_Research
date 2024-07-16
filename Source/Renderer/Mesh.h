#pragma once

#include "RHI/RHI.h"

enum class MaterialAlphaMode
{
	Opaque,
	Masked,
	Blend,
};

struct Material
{
	String Name = "Unnamed Material";
	Color BaseColorFactor = Color(1, 1, 1, 1);
	Color EmissiveFactor = Color(0, 0, 0, 1);
	float MetalnessFactor = 0.0f;
	float RoughnessFactor = 1.0f;
	float AlphaCutoff = 0.5f;
	Texture* pDiffuseTexture = nullptr;
	Texture* pNormalTexture = nullptr;
	Texture* pRoughnessMetalnessTexture = nullptr;
	Texture* pEmissiveTexture = nullptr;
	MaterialAlphaMode AlphaMode;
};

struct Mesh
{
	uint32 MaterialId;

	ResourceFormat PositionsFormat = ResourceFormat::RGB32_FLOAT;
	VertexBufferView PositionStreamLocation;
	VertexBufferView UVStreamLocation;
	VertexBufferView NormalStreamLocation;
	VertexBufferView ColorsStreamLocation;
	IndexBufferView IndicesLocation;
	uint32 MeshletsLocation;
	uint32 MeshletVerticesLocation;
	uint32 MeshletTrianglesLocation;
	uint32 MeshletBoundsLocation;
	uint32 NumMeshlets;

	BoundingBox Bounds;

	Ref<Buffer> pBuffer;
	Ref<Buffer> pBLAS;
	float ScaleFactor = 1.0f;
};

struct Model
{
	int MeshIndex;
};