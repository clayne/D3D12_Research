#pragma once
#include "Core/GraphicsBuffer.h"
class Buffer;
class CommandContext;
class Texture;
class Graphics;
class CommandContext;

class SubMesh
{
	friend class Mesh;

public:
	~SubMesh();

	void Draw(CommandContext* pContext) const;
	int GetMaterialId() const { return m_MaterialId; }
	const BoundingBox& GetBounds() const { return m_Bounds; }

	VertexBufferView GetVertexBuffer() const;
	IndexBufferView GetIndexBuffer() const;

private:
	int m_Stride = 0;
	int m_MaterialId = 0;
	uint32 m_IndexCount = 0;
	uint32 m_VertexCount = 0;
	D3D12_GPU_VIRTUAL_ADDRESS m_VerticesLocation;
	D3D12_GPU_VIRTUAL_ADDRESS m_IndicesLocation;
	BoundingBox m_Bounds;
	Mesh* m_pParent;
};

struct Material
{
	Texture* pDiffuseTexture = nullptr;
	Texture* pNormalTexture = nullptr;
	Texture* pRoughnessTexture = nullptr;
	Texture* pMetallicTexture = nullptr;
	bool IsTransparent;
};

class Mesh
{
public:
	bool Load(const char* pFilePath, Graphics* pGraphics, CommandContext* pContext);
	int GetMeshCount() const { return (int)m_Meshes.size(); }
	SubMesh* GetMesh(const int index) const { return m_Meshes[index].get(); }
	const Material& GetMaterial(int materialId) const { return m_Materials[materialId]; }

	Buffer* GetData() const { return m_pGeometryData.get(); }

private:
	std::vector<std::unique_ptr<SubMesh>> m_Meshes;
	std::vector<Material> m_Materials;
	std::unique_ptr<Buffer> m_pGeometryData;
	std::vector<std::unique_ptr<Texture>> m_Textures;
	std::map<StringHash, Texture*> m_ExistingTextures;
};