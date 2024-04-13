#include "stdafx.h"
#include "RenderGraph.h"
#include "Core/Stream.h"
#include "Graphics/ImGuiRenderer.h"

#include <External/Imgui/imgui_internal.h>
#include <External/FontAwesome/IconsFontAwesome4.h>

template<typename T>
std::string BitmaskToString(T mask, const char* (*pValueToString)(T))
{
	std::string outString;
	uint32 value = (uint32)mask;

	if (value == 0)
	{
		const char* pStr = pValueToString((T)0);
		return pStr ? pStr : "NONE";
	}

	uint32 bitIndex = 0;
	uint32 valueIndex = 0;
	while (value > 0)
	{
		if (value & 1)
		{
			const char* pStr = pValueToString((T)(1 << bitIndex));
			if (pStr)
			{
				if (valueIndex > 0)
					outString += '/';
				outString += pStr;
				valueIndex++;
			}
		}
		bitIndex++;
		value >>= 1;
	}
	return outString;
}

std::string PassFlagToString(RGPassFlag flags)
{
	return BitmaskToString<RGPassFlag>(flags,
		[](RGPassFlag flag) -> const char*
		{
			switch (flag)
			{
			case RGPassFlag::None: return "None";
			case RGPassFlag::Compute: return "Compute";
			case RGPassFlag::Raster: return "Raster";
			case RGPassFlag::Copy: return "Copy";
			case RGPassFlag::NeverCull: return "Never Cull";
			default: return nullptr;
			}
		});
}

void RGGraph::DrawResourceTracker(bool& enabled) const
{
	check(m_IsCompiled);

	if (!enabled)
		return;

	if(ImGui::Begin("Resource usage", &enabled))
	{
		std::vector<const DeviceResource*> physicalResources;
		std::unordered_map<const DeviceResource*, std::vector<const RGResource*>> physicalResourceMap;
		for (const RGResource* pResource : m_Resources)
		{
			if (pResource->GetPhysical() == nullptr || pResource->IsImported)
				continue;

			if (std::find(physicalResources.begin(), physicalResources.end(), pResource->GetPhysical()) == physicalResources.end())
				physicalResources.push_back(pResource->GetPhysical());

			physicalResourceMap[pResource->GetPhysical()].push_back(pResource);
		}

		static char resourceFilter[128] = "";

		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1, 1));
		if (ImGui::BeginTable("Resource Tracker", (int)m_Passes.size() + 1, ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImGui::GetContentRegionAvail()))
		{
			ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthFixed, 250.0f);
			for (const RGPass* pPass : m_Passes)
			{
				ImGui::TableSetupColumn(pPass->GetName(), ImGuiTableColumnFlags_AngledHeader | ImGuiTableColumnFlags_WidthFixed, 17.0f);
			}
			ImGui::TableSetupScrollFreeze(1, 2);

			ImGui::TableAngledHeadersRowEx(25.0f * Math::DegreesToRadians, 220);

			const RGPass* pActivePass = nullptr;

			// Open row
			const float row_height = ImGui::TableGetHeaderRowHeight();
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers, row_height);

			const int columns_count = ImGui::TableGetColumnCount();
			for (int column_n = 0; column_n < columns_count; column_n++)
			{
				if (!ImGui::TableSetColumnIndex(column_n))
					continue;

				// Push an id to allow unnamed labels (generally accidental, but let's behave nicely with them)
				// In your own code you may omit the PushID/PopID all-together, provided you know they won't collide.
				const char* name = (ImGui::TableGetColumnFlags(column_n) & ImGuiTableColumnFlags_NoHeaderLabel) ? "" : ImGui::TableGetColumnName(column_n);
				ImGui::PushID(column_n);

				if (column_n == 0)
				{
					ImGui::TableHeader("##name");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					ImGui::InputTextWithHint("##search", "Filter...", resourceFilter, ARRAYSIZE(resourceFilter));
				}
				else
				{
					ImGui::TableHeader(name);
				}

				if (column_n > 0 && ImGui::IsItemHovered())
				{
					const RGPass* pPass = m_Passes[column_n];
					ImGui::BeginTooltip();
					ImGui::Text("%s", pPass->GetName());
					ImGui::Text("Flags: %s", PassFlagToString(pPass->Flags).c_str());
					ImGui::Text("Index: %d", pPass->ID.GetIndex());
					ImGui::EndTooltip();

					pActivePass = pPass;
				}

				ImGui::PopID();
			}


			for (const DeviceResource* pPhysical : physicalResources)
			{
				bool filterMatch = false;
				const std::vector<const RGResource*>& resources = physicalResourceMap[pPhysical];
				for (const RGResource* pResource : resources)
				{
					if (strstr(pResource->GetName(), resourceFilter))
					{
						filterMatch = true;
						break;
					}
				}

				if (!filterMatch)
					continue;
				
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text(pPhysical->GetName());

				for (const RGResource* pResource : resources)
				{
					RGPassID firstPass = pResource->FirstAccess;
					RGPassID lastPass = pResource->LastAccess;
					if (!firstPass.IsValid() || !lastPass.IsValid())
						continue;

					uint32 firstPassOffset = pResource->IsImported ? 0 : firstPass.GetIndex();
					uint32 lastPassOffset = pResource->IsExported ? (int)m_Passes.size() - 1 : lastPass.GetIndex();

					for (uint32 i = firstPassOffset; i <= lastPassOffset; ++i)
					{
						if (ImGui::TableSetColumnIndex(i + 1))
						{
							const RGPass* pPass = m_Passes[i];
							auto it = std::find_if(pPass->Accesses.begin(), pPass->Accesses.end(), [pResource](const RGPass::ResourceAccess& access) { return access.pResource == pResource; });

							ImVec4 buttonColor = ImVec4(0.3f, 0.3f, 0.3f, 0.6f);

							if (it != pPass->Accesses.end())
							{
								if (D3D::HasWriteResourceState(it->Access))
									buttonColor = ImVec4(1.0f, 0.5f, 0.1f, 0.6f);
								else
									buttonColor = ImVec4(0.0f, 0.9f, 0.3f, 0.6f);

								if (pPass == pActivePass)
									buttonColor.w = 1.0f;
							}

							ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
							ImGui::PushStyleColor(ImGuiCol_ButtonActive, buttonColor);
							buttonColor.w = 1.0f;
							ImGui::PushStyleColor(ImGuiCol_ButtonHovered, buttonColor);
							ImGui::Button("##button", ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
							ImGui::PopStyleColor(3);

							if (ImGui::IsItemHovered())
							{
								ImGui::BeginTooltip();
								ImGui::Text("%s", pResource->GetName());

								if (pResource->GetType() == RGResourceType::Texture)
								{
									const TextureDesc& desc = static_cast<const RGTexture*>(pResource)->Desc;
									ImGui::Text("Res: %dx%dx%d", desc.Width, desc.Height, desc.DepthOrArraySize);
									ImGui::Text("Fmt: %s", RHI::GetFormatInfo(desc.Format).pName);
									ImGui::Text("Mips: %d", desc.Mips);
									ImGui::Text("Size: %s", Math::PrettyPrintDataSize(RHI::GetTextureByteSize(desc.Format, desc.Width, desc.Height, desc.DepthOrArraySize)).c_str());
								}
								else if (pResource->GetType() == RGResourceType::Buffer)
								{
									const BufferDesc& desc = static_cast<const RGBuffer*>(pResource)->Desc;
									ImGui::Text("Size: %s", Math::PrettyPrintDataSize(desc.Size).c_str());
									if (desc.Format != ResourceFormat::Unknown)
										ImGui::Text("Fmt: %s", RHI::GetFormatInfo(desc.Format).pName);
									else
										ImGui::Text("Stride: %d", desc.ElementSize);
									ImGui::Text("Elements: %d", desc.NumElements());
								}

								ImGui::Text("Export: %s - Import: %s", pResource->IsExported ? "Yes" : "No", pResource->IsImported ? "Yes" : "No");

								ImGui::EndTooltip();
							}
						}
					}
				}
			}
			ImGui::EndTable();
			ImGui::PopStyleVar();
		}
	}
	ImGui::End();
}


void RGGraph::DrawPassView(bool& enabled) const
{
	if (!enabled)
		return;

	struct TreeNode
	{
		const char*			pName;
		RGPassID			Pass;
		std::vector<int>	Children;

		void DrawNode(Span<const TreeNode> nodes, const RGGraph& graph, int depth = 0) const
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns;

			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			if (Pass.IsValid())
			{
				ImGui::PushID(Pass.GetIndex());
				const RGPass* pPass = graph.m_Passes[Pass.GetIndex()];
				bool open = ImGui::TreeNodeEx(pPass->GetName(), flags);

				ImGui::TableNextColumn();
				ImGui::TextDisabled(PassFlagToString(pPass->Flags).c_str());
				ImGui::TableNextColumn();

				if (open)
				{
					for (const RGPass::ResourceAccess& access : pPass->Accesses)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn();

						ImGui::TreeNodeEx(access.pResource->GetName(), flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
						ImGui::TableNextColumn();
						ImGui::Text(D3D::ResourceStateToString(access.Access).c_str());
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			else
			{
				if (depth == 0)
					flags |= ImGuiTreeNodeFlags_DefaultOpen;

				bool open = ImGui::TreeNodeEx(pName, flags, ICON_FA_FOLDER " %s", pName);
				ImGui::TableNextColumn();
				ImGui::TextDisabled("--");
				ImGui::TableNextColumn();

				if(open)
				{
					for (int i : Children)
					{
						nodes[i].DrawNode(nodes, graph, depth + 1);
					}
					ImGui::TreePop();
				}
			}
		}
	};

	std::vector<TreeNode> nodes(1);
	std::vector<int> nodeStack;
	nodeStack.push_back(0);

	for (RGPass* pPass : m_Passes)
	{
		if (pPass->IsCulled)
			continue;

		for (RGEventID eventID : pPass->EventsToStart)
		{
			uint32 newIndex = (uint32)nodes.size();
			nodes[nodeStack.back()].Children.push_back(newIndex);
			TreeNode& newNode = nodes.emplace_back();
			newNode.pName = m_Events[eventID.GetIndex()].pName;
			nodeStack.push_back(newIndex);
		}

		uint32 newIndex = (uint32)nodes.size();
		TreeNode& newNode = nodes.emplace_back();
		nodes[nodeStack.back()].Children.push_back(newIndex);
		newNode.Pass = pPass->ID;

		for (uint32 i = 0; i < pPass->NumEventsToEnd; ++i)
		{
			nodeStack.pop_back();
		}
	}

	check(nodeStack.size() == 1);

	Span<int> rootNodes = nodes[0].Children;

	if (ImGui::Begin("Passes", &enabled))
	{
		if (ImGui::BeginTable("Passes", 2, ImGuiTableFlags_Resizable))
		{
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Resources");
			ImGui::TableHeadersRow();

			for (int node : rootNodes)
			{
				nodes[node].DrawNode(nodes, *this);
			}

			ImGui::EndTable();
		}
	}
	ImGui::End();
}


void RGGraph::DumpDebugGraph(const char* pPath) const
{
	check(m_IsCompiled);

	struct StringStream
	{
		StringStream& operator<<(const char* pText)
		{
			if(String.size() + strlen(pText) > String.capacity())
				String.reserve(String.capacity() * 2);

			String += pText;
			return *this;
		}

		StringStream& operator<<(const std::string& text)
		{
			return operator<<(text.c_str());
		}

		StringStream& operator<<(int v)
		{
			return operator<<(Sprintf("%d", v));
		}

		std::string String;
	};

	uint32 neverCullColor			= 0xFF5E00FF;
	uint32 referencedPassColor		= 0xFFAA00FF;
	uint32 unreferedPassColor		= 0xFFEEEEFF;
	uint32 referencedResourceColor	= 0xBBEEFFFF;
	uint32 importedResourceColor	= 0x99BBDDFF;

	// Mermaid
	{
		StringStream stream;

		constexpr const char* pMermaidTemplate = R"(
			<!DOCTYPE html>
				<html lang="en">
				<head>
					<meta charset="utf-8">
					<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.1.1/css/all.min.css"
						integrity="sha512-KfkfwYDsLkIlwQp6LFnl8zNdLGxu9YAA1QvwINks4PhcElQSvqcyVLLD9aMhXd13uQjoXtEKNosOWaZqXgel0g=="
						crossorigin="anonymous" referrerpolicy="no-referrer" />
				</head>
				<body>
					<script src="https://cdn.jsdelivr.net/npm/mermaid/dist/mermaid.min.js"></script>
					<script>
						mermaid.initialize({ startOnLoad: true, maxTextSize: 90000, flowchart: { useMaxWidth: false, htmlLabels: true }});
					</script>
					<div class="mermaid">
						%s
					</div>
				</body>
			</html>
		)";

		stream << "graph TD;\n";

		stream << Sprintf("classDef neverCullPass fill:#%x,stroke:#333,stroke-width:4px;\n", neverCullColor);
		stream << Sprintf("classDef referencedPass fill:#%x,stroke:#333,stroke-width:4px;\n", referencedPassColor);
		stream << Sprintf("classDef unreferenced stroke:#fee,stroke-width:1px;\n");
		stream << Sprintf("classDef referencedResource fill:#%x,stroke:#333,stroke-width:2px;\n", referencedResourceColor);
		stream << Sprintf("classDef importedResource fill:#%x,stroke:#333,stroke-width:2px;\n", importedResourceColor);

		const char* writeLinkStyle = "stroke:#f82,stroke-width:2px;";
		const char* readLinkStyle = "stroke:#9c9,stroke-width:2px;";
		int linkIndex = 0;

		std::unordered_map<RGResource*, uint32> resourceVersions;

		//Pass declaration
		int passIndex = 0;
		for (RGPass* pPass : m_Passes)
		{
			stream << "Pass" << pPass->ID.GetIndex();
			stream << "[";
			stream << "\"" << pPass->GetName() << "\"<br/>";
			stream << "Flags: " << PassFlagToString(pPass->Flags) << "<br/>";
			stream << "Index: " << passIndex << "<br/>";
			stream << "Culled: " << (pPass->IsCulled ? "Yes" : "No") << "<br/>";
			stream << "]:::";

			if (EnumHasAnyFlags(pPass->Flags, RGPassFlag::NeverCull))
			{
				stream << "neverCullPass";
			}
			else
			{
				stream << (pPass->IsCulled ? "unreferenced" : "referencedPass");
			}

			stream << "\n";

			auto PrintResource = [&](RGResource* pResource, uint32 version) {
				stream << "Resource" << pResource->ID.GetIndex() << "_" << version;
				stream << (pResource->IsImported ? "[(" : "([");
				stream << "\"" << pResource->GetName() << "\"<br/>";

				if (pResource->GetType() == RGResourceType::Texture)
				{
					const TextureDesc& desc = static_cast<RGTexture*>(pResource)->Desc;
					stream << "Res: " << desc.Width << "x" << desc.Height << "x" << desc.DepthOrArraySize << "<br/>";
					stream << "Fmt: " << RHI::GetFormatInfo(desc.Format).pName << "<br/>";
					stream << "Mips: " << desc.Mips << "<br/>";
					stream << "Size: " << Math::PrettyPrintDataSize(RHI::GetTextureByteSize(desc.Format, desc.Width, desc.Height, desc.DepthOrArraySize)) << "</br>";
				}
				else if (pResource->GetType() == RGResourceType::Buffer)
				{
					const BufferDesc& desc = static_cast<RGBuffer*>(pResource)->Desc;
					stream << "Stride: " << desc.ElementSize << "<br/>";
					stream << "Fmt: " << RHI::GetFormatInfo(desc.Format).pName << "<br/>";
					stream << "Size: " << Math::PrettyPrintDataSize(desc.Size) << "<br/>";
					stream << "Elements: " << desc.NumElements() << "<br/>";
				}

				stream << (pResource->IsImported ? ")]" : "])");
				if (pResource->IsImported)
				{
					stream << ":::importedResource";
				}
				else
				{
					stream << ":::referencedResource";
				}
				stream << "\n";
				};

			for (RGPass::ResourceAccess& access : pPass->Accesses)
			{
				RGResource* pResource = access.pResource;
				uint32 resourceVersion = 0;
				auto it = resourceVersions.find(pResource);
				if (it == resourceVersions.end())
				{
					resourceVersions[pResource] = resourceVersion;

					if (pResource->IsImported)
						PrintResource(pResource, resourceVersion);

				}
				resourceVersion = resourceVersions[pResource];

				if (resourceVersion > 0 || pResource->IsImported)
				{
					stream << "Resource" << pResource->ID.GetIndex() << "_" << resourceVersion << " -- " << D3D::ResourceStateToString(access.Access) << " --> Pass" << pPass->ID.GetIndex() << "\n";
					stream << "linkStyle " << linkIndex++ << " " << readLinkStyle << "\n";
				}

				if (D3D::HasWriteResourceState(access.Access))
				{
					++resourceVersions[pResource];
					resourceVersion++;
					PrintResource(pResource, resourceVersion);

					stream << "Pass" << pPass->ID.GetIndex() << " -- " << D3D::ResourceStateToString(access.Access) << " --> " << "Resource" << pResource->ID.GetIndex() << "_" << resourceVersion;
					stream << "\nlinkStyle " << linkIndex++ << " " << writeLinkStyle << "\n";
				}
			}

			++passIndex;
		}

		std::string output = Sprintf(pMermaidTemplate, stream.String.c_str());

		std::string fullPath = Paths::MakeAbsolute(Sprintf("%s.html", pPath).c_str());
		Paths::CreateDirectoryTree(fullPath);

		FileStream file;
		if (file.Open(fullPath.c_str(), FileMode::Write))
			file.Write(output.c_str(), (uint32)output.length());
	}

	// GraphViz
	{
		const char* pGraphVizTemplate = R"(<div id="graph"></div>
			<script src="https://cdn.jsdelivr.net/npm/@viz-js/viz@3.4.0/lib/viz-standalone.js"></script>
			<script>
			  Viz.instance().then(function(viz) {
			    var svg = viz.renderSVGElement(`%s`);

			    document.getElementById("graph").appendChild(svg);
			  });
			</script>)";

		std::unordered_map<RGResource*, uint32> resourceVersions;

		StringStream stream;

		auto PrintResource = [&](RGResource* pResource, uint32 version) {
			stream << "Resource" << pResource->ID.GetIndex() << "_" << version;
			stream << "[ label = \"" << pResource->GetName() << "\\n";

			if (pResource->GetType() == RGResourceType::Texture)
			{
				const TextureDesc& desc = static_cast<RGTexture*>(pResource)->Desc;
				stream << "Res: " << desc.Width << "x" << desc.Height << "x" << desc.DepthOrArraySize << "\\n";
				stream << "Fmt: " << RHI::GetFormatInfo(desc.Format).pName << "\\n";
				stream << "Mips: " << desc.Mips << "\\n";
				stream << "Size: " << Math::PrettyPrintDataSize(RHI::GetTextureByteSize(desc.Format, desc.Width, desc.Height, desc.DepthOrArraySize));
			}
			else if (pResource->GetType() == RGResourceType::Buffer)
			{
				const BufferDesc& desc = static_cast<RGBuffer*>(pResource)->Desc;
				stream << "Stride: " << desc.ElementSize << "\\n";
				stream << "Fmt: " << RHI::GetFormatInfo(desc.Format).pName << "\\n";
				stream << "Size: " << Math::PrettyPrintDataSize(desc.Size) << "\\n";
				stream << "Elements: " << desc.NumElements();
			}

			uint32 color = pResource->IsImported ? importedResourceColor : referencedResourceColor;
			const char* pShape = pResource->IsImported ? "cylinder" : "oval";
			stream << Sprintf("\" penwidth=2 shape=%s style=filled fillcolor=\"#%x\" ];\n", pShape, color);
			};

		stream << "digraph {\n";

		stream << "splines=ortho;\n";

		int passIndex = 0;
		for (RGPass* pPass : m_Passes)
		{
			uint32 passColor = referencedPassColor;
			if (EnumHasAnyFlags(pPass->Flags, RGPassFlag::NeverCull))
				passColor = neverCullColor;
			else if (pPass->IsCulled)
				passColor = unreferedPassColor;

			stream << "Pass" << pPass->ID.GetIndex() << " ";
			stream << "[ label = ";
			stream << "\"" << pPass->GetName() << "\\n";
			stream << "Flags: " << PassFlagToString(pPass->Flags) << "\\n";
			stream << "Index: " << passIndex << "\\n";
			stream << "Culled: " << (pPass->IsCulled ? "Yes" : "No");
			stream << Sprintf("\" penwidth=4 shape=rectangle style=filled fillcolor=\"#%x\"];", passColor);

			stream << "\n";

			for (RGPass::ResourceAccess& access : pPass->Accesses)
			{
				RGResource* pResource = access.pResource;
				uint32 resourceVersion = 0;
				auto it = resourceVersions.find(pResource);
				if (it == resourceVersions.end())
				{
					resourceVersions[pResource] = resourceVersion;

					if (pResource->IsImported)
						PrintResource(pResource, resourceVersion);
				}
				resourceVersion = resourceVersions[pResource];

				if (resourceVersion > 0 || pResource->IsImported)
				{
					stream << "Resource" << pResource->ID.GetIndex() << "_" << resourceVersion << " -> " << "Pass" << pPass->ID.GetIndex() << "\n";
				}

				if (D3D::HasWriteResourceState(access.Access))
				{
					++resourceVersions[pResource];
					resourceVersion++;
					PrintResource(pResource, resourceVersion);

					stream << "Pass" << pPass->ID.GetIndex() << " -> " << "Resource" << pResource->ID.GetIndex() << "_" << resourceVersion << "\n";
				}
			}

			++passIndex;
		}

		stream << "}\n";

		std::string fullPath = Paths::MakeAbsolute(Sprintf("%s_GraphViz.html", pPath).c_str());
		Paths::CreateDirectoryTree(fullPath);

		std::string output = Sprintf(pGraphVizTemplate, stream.String);
		FileStream file;
		if (file.Open(fullPath.c_str(), FileMode::Write))
			file.Write(output.c_str(), (uint32)output.length());

		ShellExecuteA(nullptr, "open", fullPath.c_str(), nullptr, nullptr, SW_SHOW);
	}
}
