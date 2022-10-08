#include "Common.hlsli"

struct CharacterInstance
{
	float2 Position;
	uint Character;
	uint Color;
};

struct LineInstance
{
	float3 A;
	float3 B;
	uint Color;
};

static const uint TEXT_COUNTER_OFFSET = 0;
static const uint LINE_COUNTER_OFFSET = 1;
static const uint DATA_OFFSET = 4;
static const uint TEXT_INSTANCES_OFFSET = DATA_OFFSET;
static const uint LINE_INSTANCES_OFFSET = TEXT_INSTANCES_OFFSET + 256 * sizeof(CharacterInstance);

struct Glyph
{
	uint2 Location;
	int2 Offset;
	uint2 Dimensions;
	uint Width;
};

void DrawChar(CharacterInstance instance)
{
	RWStructuredBuffer<uint> renderData = ResourceDescriptorHeap[cView.DebugRenderDataIndex];
	uint offset;
	InterlockedAdd(renderData[TEXT_COUNTER_OFFSET], 1, offset);
	offset *= sizeof(CharacterInstance);
	offset += TEXT_INSTANCES_OFFSET;
	renderData[offset++] = instance.Position.x;
	renderData[offset++] = instance.Position.y;
	renderData[offset++] = instance.Character;
	renderData[offset++] = instance.Color;
}

CharacterInstance GetChar(uint index, StructuredBuffer<uint> renderData)
{
	CharacterInstance instance;
	uint offset = index * sizeof(CharacterInstance) + TEXT_INSTANCES_OFFSET;
	instance.Position.x = renderData[offset++];
	instance.Position.y = renderData[offset++];
	instance.Character = renderData[offset++];
	instance.Color = renderData[offset++];
	return instance;
}

void DrawLine(float3 a, float3 b, uint color = 0xFFFFFFFF)
{
	RWStructuredBuffer<uint> renderData = ResourceDescriptorHeap[cView.DebugRenderDataIndex];
	uint offset;
	InterlockedAdd(renderData[LINE_COUNTER_OFFSET], 1, offset);
	offset *= sizeof(LineInstance);
	offset += LINE_INSTANCES_OFFSET;
	renderData[offset++] = asuint(a.x);
	renderData[offset++] = asuint(a.y);
	renderData[offset++] = asuint(a.z);
	renderData[offset++] = asuint(b.x);
	renderData[offset++] = asuint(b.y);
	renderData[offset++] = asuint(b.z);
	renderData[offset++] = color;
}

LineInstance GetLine(uint index, StructuredBuffer<uint> renderData)
{
	LineInstance instance;
	uint offset = index * sizeof(LineInstance) + LINE_INSTANCES_OFFSET;
	instance.A.x = asfloat(renderData[offset++]);
	instance.A.y = asfloat(renderData[offset++]);
	instance.A.z = asfloat(renderData[offset++]);
	instance.B.x = asfloat(renderData[offset++]);
	instance.B.y = asfloat(renderData[offset++]);
	instance.B.z = asfloat(renderData[offset++]);
	instance.Color = renderData[offset++];
	return instance;
}

struct TextWriter
{
	float2 StartLocation;
	float2 CursorLocation;

	void Text(uint character, uint color)
	{
		StructuredBuffer<Glyph> glyphBuffer = ResourceDescriptorHeap[cView.FontDataIndex];
		Glyph glyph = glyphBuffer[character];

		CharacterInstance instance;
		instance.Position = CursorLocation + int2(-glyph.Offset.x, glyph.Offset.y);
		instance.Character = character;
		instance.Color = color;
		DrawChar(instance);

		CursorLocation.x += glyph.Width;
	}

	void NewLine()
	{
		StructuredBuffer<Glyph> glyphBuffer = ResourceDescriptorHeap[cView.FontDataIndex];
		Glyph glyph = glyphBuffer[0];
		CursorLocation.y += glyph.Dimensions.y;
		CursorLocation.x = StartLocation.x;
	}

	void Text(uint a, uint b, uint color)
	{
		Text(a, color);
		Text(b, color);
	}

	void Text(uint a, uint b, uint c, uint color)
	{
		Text(a, b, color);
		Text(c, color);
	}

	void Text(uint a, uint b, uint c, uint d, uint color)
	{
		Text(a, b, color);
		Text(c, d, color);
	}

	void Text(uint a, uint b, uint c, uint d, uint e, uint color)
	{
		Text(a, b, c, color);
		Text(d, e, color);
	}

	void Text(uint a, uint b, uint c, uint d, uint e, uint f, uint color)
	{
		Text(a, b, c, color);
		Text(d, e, f, color);
	}

	void Text(uint a, uint b, uint c, uint d, uint e, uint f, uint g, uint color)
	{
		Text(a, b, c, color);
		Text(d, e, f, g, color);
	}

	void Int(int value, uint color)
	{
		if(value < 0)
		{
			Text('-', color);
			value = -value;
		}
		uint length = value > 0 ? log10(value) + 1 : 1;
		uint divider = round(pow(10, length - 1));

		while(length > 0)
		{
			uint digit = value / divider;
			Text('0' + digit, color);
			--length;

			value = value - digit * divider;
			divider /= 10;
		}
	}

	void Float(float value, uint color)
	{
		if(isnan(value))
		{
			Text('N', 'a', 'N', color);
		}
		else if(!isfinite(value))
		{
			Text('I', 'N', 'F', color);
		}
		else
		{
			int v0 = floor(abs(value));
			Int(sign(value) * v0, color);
			Text('.', color);
			int v1 = floor(frac(value) * 10000);
			Int(v1, color);
		}
	}
};

TextWriter CreateTextWriter(float2 position)
{
	TextWriter writer;
	writer.StartLocation = position;
	writer.CursorLocation = position;
	return writer;
}
