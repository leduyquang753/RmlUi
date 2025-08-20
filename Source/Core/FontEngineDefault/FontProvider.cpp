/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019-2023 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "FontProvider.h"
#include "../../../Include/RmlUi/Core/Core.h"
#include "../../../Include/RmlUi/Core/FileInterface.h"
#include "../../../Include/RmlUi/Core/Log.h"
#include "../../../Include/RmlUi/Core/Math.h"
#include "../../../Include/RmlUi/Core/StringUtilities.h"
#include "../ComputeProperty.h"
#include "FontFace.h"
#include "FontFaceHandleDefault.h"
#include "FontFamily.h"
#include "FreeTypeInterface.h"
#include <algorithm>

namespace Rml {

static FontProvider* g_font_provider = nullptr;

FontProvider::FontProvider()
{
	RMLUI_ASSERT(!g_font_provider);
}

FontProvider::~FontProvider()
{
	RMLUI_ASSERT(g_font_provider == this);
}

bool FontProvider::Initialise()
{
	RMLUI_ASSERT(!g_font_provider);
	if (!FreeType::Initialise())
		return false;
	g_font_provider = new FontProvider;
	return true;
}

void FontProvider::Shutdown()
{
	RMLUI_ASSERT(g_font_provider);
	delete g_font_provider;
	g_font_provider = nullptr;
	FreeType::Shutdown();
}

void FontProvider::OnBeginFrame()
{
	Get().OnBeginFrameInternal();
}

void FontProvider::OnBeginFrameInternal()
{
	//.sprite_set.Tick();
	for (auto iterator = font_families.begin(); iterator != font_families.end(); ++iterator)
		iterator->second->OnBeginFrame();
	glyph_lru_list.tick();
	while (glyph_lru_list.getLastEntryAge() > 600)
	{
		const auto& entry = *glyph_lru_list.getLast();
		entry.font_face->RemoveGlyph(entry.font_effects_handle, entry.character, sprite_set);
		glyph_lru_list.evictLast();
	}
}

FontProvider& FontProvider::Get()
{
	RMLUI_ASSERT(g_font_provider);
	return *g_font_provider;
}

FontFaceHandleDefault* FontProvider::GetFontFaceHandle(const String& family, Style::FontStyle style, Style::FontWeight weight, int size)
{
	RMLUI_ASSERTMSG(family == StringUtilities::ToLower(family), "Font family name must be converted to lowercase before entering here.");

	FontFamilyMap& families = Get().font_families;

	auto it = families.find(family);
	if (it == families.end())
		return nullptr;

	return it->second->GetFaceHandle(style, weight, size);
}

int FontProvider::CountFallbackFontFaces()
{
	return (int)Get().fallback_font_faces.size();
}

FontFaceHandleDefault* FontProvider::GetFallbackFontFace(int index, int font_size)
{
	auto& faces = FontProvider::Get().fallback_font_faces;

	if (index >= 0 && index < (int)faces.size())
		return faces[index]->GetHandle(font_size, false);

	return nullptr;
}

void FontProvider::ReleaseFontResources()
{
	Get().ReleaseFontResourcesInternal();
}

void FontProvider::ReleaseFontResourcesInternal()
{
	for (auto& name_family : font_families)
		name_family.second->ReleaseFontResources();
	sprite_set = {4, texture_size, 1};
	render_textures.clear();
	glyph_lru_list = {};
}

bool FontProvider::LoadFontFace(const String& file_name, int face_index, bool fallback_face, Style::FontWeight weight)
{
	FileInterface* file_interface = GetFileInterface();
	FileHandle handle = file_interface->Open(file_name);

	if (!handle)
	{
		Log::Message(Log::LT_ERROR, "Failed to load font face from %s, could not open file.", file_name.c_str());
		return false;
	}

	size_t length = file_interface->Length(handle);

	auto buffer_ptr = UniquePtr<byte[]>(new byte[length]);
	byte* buffer = buffer_ptr.get();
	file_interface->Read(buffer, length, handle);
	file_interface->Close(handle);

	bool result = Get().LoadFontFace({buffer, length}, face_index, fallback_face, std::move(buffer_ptr), file_name, {}, Style::FontStyle::Normal, weight);

	return result;
}

bool FontProvider::LoadFontFace(Span<const byte> data, int face_index, const String& font_family, Style::FontStyle style, Style::FontWeight weight,
	bool fallback_face)
{
	const String source = "memory";

	bool result = Get().LoadFontFace(data, face_index, fallback_face, nullptr, source, font_family, style, weight);

	return result;
}

bool FontProvider::LoadFontFace(Span<const byte> data, int face_index, bool fallback_face, UniquePtr<byte[]> face_memory, const String& source, String font_family,
	Style::FontStyle style, Style::FontWeight weight)
{
	using Style::FontWeight;

	Vector<FaceVariation> face_variations;
	if (!FreeType::GetFaceVariations(data, face_variations, face_index))
	{
		Log::Message(Log::LT_ERROR, "Failed to load font face from '%s': Invalid or unsupported font face file format.", source.c_str());
		return false;
	}

	Vector<FaceVariation> load_variations;
	if (face_variations.empty())
	{
		load_variations.push_back(FaceVariation{Style::FontWeight::Auto, 0, 0});
	}
	else
	{
		// Iterate through all the face variations and pick the ones to load. The list is already sorted by (weight, width). When weight is set to
		// 'auto' we load all the weights of the face. However, we only want to load one width for each weight.
		for (auto it = face_variations.begin(); it != face_variations.end();)
		{
			if (weight != FontWeight::Auto && it->weight != weight)
			{
				++it;
				continue;
			}

			// We don't currently have any way for users to select widths, so we search for a regular (medium) value here.
			constexpr int search_width = 100;
			const FontWeight current_weight = it->weight;

			int best_width_distance = Math::Absolute((int)it->width - search_width);
			auto it_best_width = it;

			// Search forward to find the best 'width' with the same weight.
			for (++it; it != face_variations.end(); ++it)
			{
				if (it->weight != current_weight)
					break;

				const int width_distance = Math::Absolute((int)it->width - search_width);
				if (width_distance < best_width_distance)
				{
					best_width_distance = width_distance;
					it_best_width = it;
				}
			}

			load_variations.push_back(*it_best_width);
		}
	}

	if (load_variations.empty())
	{
		Log::Message(Log::LT_ERROR, "Failed to load font face from '%s': Could not locate face with weight %d.", source.c_str(), (int)weight);
		return false;
	}

	for (const FaceVariation& variation : load_variations)
	{
		FontFaceHandleFreetype ft_face = FreeType::LoadFace(data, source, face_index, variation.named_instance_index);
		if (!ft_face)
			return false;

		if (font_family.empty())
			FreeType::GetFaceStyle(ft_face, &font_family, &style, nullptr);
		if (weight == FontWeight::Auto)
			FreeType::GetFaceStyle(ft_face, nullptr, nullptr, &weight);

		const FontWeight variation_weight = (variation.weight == FontWeight::Auto ? weight : variation.weight);
		const String font_face_description = GetFontFaceDescription(font_family, style, variation_weight);

		if (!AddFace(ft_face, font_family, style, variation_weight, fallback_face, std::move(face_memory)))
		{
			Log::Message(Log::LT_ERROR, "Failed to load font face %s from '%s'.", font_face_description.c_str(), source.c_str());
			return false;
		}

		Log::Message(Log::LT_INFO, "Loaded font face %s from '%s'.", font_face_description.c_str(), source.c_str());
	}

	return true;
}

bool FontProvider::AddFace(FontFaceHandleFreetype face, const String& family, Style::FontStyle style, Style::FontWeight weight, bool fallback_face,
	UniquePtr<byte[]> face_memory)
{
	if (family.empty() || weight == Style::FontWeight::Auto)
		return false;

	String family_lower = StringUtilities::ToLower(family);
	FontFamily* font_family = nullptr;
	auto it = font_families.find(family_lower);
	if (it != font_families.end())
	{
		font_family = (FontFamily*)it->second.get();
	}
	else
	{
		auto font_family_ptr = MakeUnique<FontFamily>(family_lower);
		font_family = font_family_ptr.get();
		font_families[family_lower] = std::move(font_family_ptr);
	}

	FontFace* font_face_result = font_family->AddFace(face, style, weight, std::move(face_memory));

	if (font_face_result && fallback_face)
	{
		auto it_fallback_face = std::find(fallback_font_faces.begin(), fallback_font_faces.end(), font_face_result);
		if (it_fallback_face == fallback_font_faces.end())
		{
			fallback_font_faces.push_back(font_face_result);
		}
	}

	return static_cast<bool>(font_face_result);
}

bool FontProvider::EnsureGlyphs(FontFaceHandle handle, FontEffectsHandle font_effects_handle, StringView string)
{
	return Get().EnsureGlyphsInternal(handle, font_effects_handle, string);
}

bool FontProvider::EnsureGlyphsInternal(FontFaceHandle handle, FontEffectsHandle font_effects_handle, StringView string)
{
	auto handle_default = reinterpret_cast<FontFaceHandleDefault*>(handle);
	return handle_default->EnsureGlyphs(string, static_cast<int>(font_effects_handle), glyph_lru_list);
}

int FontProvider::GenerateString(
	RenderManager& render_manager, FontFaceHandle handle, FontEffectsHandle font_effects_handle,
	StringView string, Vector2f position, ColourbPremultiplied colour, float opacity, const TextShapingContext& text_shaping_context,
	TexturedMeshList& mesh_list)
{
	return Get().GenerateStringInternal(render_manager, handle, font_effects_handle, string, position, colour, opacity, text_shaping_context, mesh_list);
}

int FontProvider::GenerateStringInternal(
	RenderManager& render_manager, FontFaceHandle handle, FontEffectsHandle font_effects_handle,
	StringView string, Vector2f position, ColourbPremultiplied colour, float opacity, const TextShapingContext& text_shaping_context,
	TexturedMeshList& mesh_list)
{
	const auto handle_default = reinterpret_cast<FontFaceHandleDefault*>(handle);
	const int layer_configuration = static_cast<int>(font_effects_handle);
	if (handle_default->LoadGlyphsForString(string, layer_configuration, sprite_set, glyph_lru_list))
		FlushTextureAtlases();
	return handle_default->GenerateString(
		render_manager, sprite_set, render_textures, mesh_list, string, position, colour, opacity,
		text_shaping_context.letter_spacing, layer_configuration
	);
}

void FontProvider::FlushTextureAtlases()
{
	const Vector<SpriteSet::TextureInfo> texture_infos = sprite_set.GetTextures();
	const int texture_count = static_cast<int>(texture_infos.size());
	for (int texture_id = 0; texture_id < texture_count; ++texture_id)
	{
		const SpriteSet::TextureInfo& texture_info = texture_infos[texture_id];
		if (texture_id < render_textures.size() && texture_info.first_dirty_y >= texture_info.past_last_dirty_y)
			continue;
		const unsigned char* const texture_data = texture_info.texture_data;
		CallbackTextureFunction texture_callback
			= [texture_data](const CallbackTextureInterface& texture_interface) -> bool {
			return texture_interface.GenerateTexture(
				{texture_data, texture_size * texture_size * 4}, {texture_size, texture_size}
			);
		};

		static_assert(std::is_nothrow_move_constructible<CallbackTextureSource>::value,
			"CallbackTextureSource must be nothrow move constructible so that it can be placed in the vector below.");
		if (texture_id >= render_textures.size())
			render_textures.emplace_back(std::move(texture_callback));
		else
			render_textures[texture_id] = {std::move(texture_callback)};
	}
}

} // namespace Rml
