/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "render/scene_data.h"

#include "image_loader.h"
#include "render/gpu_buffer.h"
#include "utils/fmt_glm.h"
#include "utils/ranges.h"
#include <boost/pfr/core.hpp>
#include <concepts>
#include <fastgltf/base64.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/util.hpp>
#include <fstream>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>

#include "application.h"
#include "asset.h"

template <class T>
constexpr vk::Format vk_attribute_format = vk::Format::eUndefined;
template <>
constexpr vk::Format vk_attribute_format<float> = vk::Format::eR32Sfloat;
template <>
constexpr vk::Format vk_attribute_format<glm::vec2> = vk::Format::eR32G32Sfloat;
template <>
constexpr vk::Format vk_attribute_format<glm::vec3> = vk::Format::eR32G32B32Sfloat;
template <>
constexpr vk::Format vk_attribute_format<glm::vec4> = vk::Format::eR32G32B32A32Sfloat;
template <typename T, size_t N>
constexpr vk::Format vk_attribute_format<std::array<T, N>> = vk_attribute_format<T>;

template <class T>
constexpr size_t nb_attributes = 1;
template <typename T, size_t N>
constexpr size_t nb_attributes<std::array<T, N>> = N;

template <class T>
struct remove_array
{
	using type = T;
};
template <class T, size_t N>
struct remove_array<std::array<T, N>>
{
	using type = T;
};
template <class T>
using remove_array_t = remove_array<T>::type;

template <class T>
struct is_array : std::false_type
{
};

template <class T, size_t N>
struct is_array<std::array<T, N>> : std::true_type
{
};

template <class T>
constexpr bool is_array_v = is_array<T>::value;

scene_data::vertex::description scene_data::vertex::describe()
{
	scene_data::vertex::description desc;

	desc.binding = vk::VertexInputBindingDescription{
	        .binding = 0,
	        .stride = sizeof(vertex),
	        .inputRate = vk::VertexInputRate::eVertex,
	};

	vk::VertexInputAttributeDescription attribute{
	        .location = 0,
	        .binding = 0,
	};

#define VERTEX_ATTR(member)                                                                                         \
	attribute.format = vk_attribute_format<decltype(vertex::member)>;                                           \
	for (size_t i = 0; i < nb_attributes<decltype(vertex::member)>; i++)                                        \
	{                                                                                                           \
		attribute.offset = offsetof(vertex, member) + i * sizeof(remove_array_t<decltype(vertex::member)>); \
		desc.attributes.push_back(attribute);                                                               \
		attribute.location++;                                                                               \
		if (is_array_v<decltype(vertex::member)>)                                                           \
		{                                                                                                   \
			desc.attribute_names.push_back(#member "_" + std::to_string(i));                            \
		}                                                                                                   \
		else                                                                                                \
		{                                                                                                   \
			desc.attribute_names.push_back(#member);                                                    \
		}                                                                                                   \
	}

	VERTEX_ATTR(position);
	VERTEX_ATTR(normal);
	VERTEX_ATTR(tangent);
	VERTEX_ATTR(texcoord);
	VERTEX_ATTR(color);
	VERTEX_ATTR(joints);
	VERTEX_ATTR(weights);

	static_assert(vertex{}.joints.size() == vertex{}.weights.size());

	return desc;
}

// Conversion functions gltf -> vulkan
namespace
{
fastgltf::Asset load_gltf_asset(fastgltf::GltfDataBuffer & buffer, const std::filesystem::path & directory)
{
	fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu);

	auto gltf_options =
	        fastgltf::Options::DontRequireValidAssetMember |
	        fastgltf::Options::AllowDouble |
	        fastgltf::Options::LoadGLBBuffers |
	        // fastgltf::Options::LoadExternalBuffers |
	        // fastgltf::Options::LoadExternalImages |
	        fastgltf::Options::DecomposeNodeMatrices;

	fastgltf::Expected<fastgltf::Asset> expected_asset{fastgltf::Error::None};

	switch (fastgltf::determineGltfFileType(&buffer))
	{
		case fastgltf::GltfType::GLB:
			expected_asset = parser.loadBinaryGLTF(&buffer, directory, gltf_options);
			break;

		case fastgltf::GltfType::glTF:
			expected_asset = parser.loadGLTF(&buffer, directory, gltf_options);
			break;

		case fastgltf::GltfType::Invalid:
			throw std::runtime_error("Unrecognized file type");
	}

	if (auto error = expected_asset.error(); error != fastgltf::Error::None)
		throw std::runtime_error(std::string(fastgltf::getErrorMessage(error)));

	return std::move(expected_asset.get());
}

std::pair<vk::Filter, vk::SamplerMipmapMode> convert(fastgltf::Filter filter)
{
	switch (filter)
	{
		case fastgltf::Filter::Nearest:
		case fastgltf::Filter::NearestMipMapNearest:
			return {vk::Filter::eNearest, vk::SamplerMipmapMode::eNearest};

		case fastgltf::Filter::Linear:
		case fastgltf::Filter::LinearMipMapNearest:
			return {vk::Filter::eLinear, vk::SamplerMipmapMode::eNearest};

		case fastgltf::Filter::NearestMipMapLinear:
			return {vk::Filter::eNearest, vk::SamplerMipmapMode::eLinear};

		case fastgltf::Filter::LinearMipMapLinear:
			return {vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear};
	}

	throw std::invalid_argument("filter");
}

vk::SamplerAddressMode convert(fastgltf::Wrap wrap)
{
	switch (wrap)
	{
		case fastgltf::Wrap::ClampToEdge:
			return vk::SamplerAddressMode::eClampToEdge;

		case fastgltf::Wrap::MirroredRepeat:
			return vk::SamplerAddressMode::eMirroredRepeat;

		case fastgltf::Wrap::Repeat:
			return vk::SamplerAddressMode::eRepeat;
	}

	throw std::invalid_argument("wrap");
}

sampler_info convert(fastgltf::Sampler & sampler)
{
	sampler_info info;

	info.mag_filter = convert(sampler.magFilter.value_or(fastgltf::Filter::Linear)).first;

	std::tie(info.min_filter, info.min_filter_mipmap) = convert(sampler.minFilter.value_or(fastgltf::Filter::LinearMipMapLinear));

	info.wrapS = convert(sampler.wrapS);
	info.wrapT = convert(sampler.wrapT);

	return info;
}

vk::PrimitiveTopology convert(fastgltf::PrimitiveType type)
{
	switch (type)
	{
		case fastgltf::PrimitiveType::Points:
			return vk::PrimitiveTopology::ePointList;

		case fastgltf::PrimitiveType::Lines:
			return vk::PrimitiveTopology::eLineList;

		case fastgltf::PrimitiveType::LineLoop:
			throw std::runtime_error("Unimplemented");

		case fastgltf::PrimitiveType::LineStrip:
			return vk::PrimitiveTopology::eLineStrip;

		case fastgltf::PrimitiveType::Triangles:
			return vk::PrimitiveTopology::eTriangleList;

		case fastgltf::PrimitiveType::TriangleStrip:
			return vk::PrimitiveTopology::eTriangleStrip;

		case fastgltf::PrimitiveType::TriangleFan:
			return vk::PrimitiveTopology::eTriangleFan;
	}

	throw std::invalid_argument("type");
}

glm::vec4 convert(const std::array<float, 4> & v)
{
	return {v[0], v[1], v[2], v[3]};
}

glm::vec3 convert(const std::array<float, 3> & v)
{
	return {v[0], v[1], v[2]};
}
} // namespace

template <typename T>
        requires is_array_v<T>
void copy_vertex_attributes(
        const fastgltf::Asset & asset,
        const fastgltf::Primitive & primitive,
        std::string attribute_name,
        std::vector<scene_data::vertex> & vertices,
        T scene_data::vertex::*attribute)
{
	using U = T::value_type;
	constexpr size_t N = std::tuple_size_v<T>;

	for (size_t i = 0; i < N; i++)
	{
		auto it = primitive.findAttribute(attribute_name + std::to_string(i));

		if (it == primitive.attributes.end())
			continue;

		const fastgltf::Accessor & accessor = asset.accessors.at(it->second);

		if (vertices.size() < accessor.count)
			vertices.resize(accessor.count, {});

		fastgltf::iterateAccessorWithIndex<U>(asset, accessor, [&](U value, std::size_t idx) {
			(vertices[idx].*attribute)[i] = value;
		});
	}
}

template <typename T>
        requires(!is_array_v<T>)
void copy_vertex_attributes(
        const fastgltf::Asset & asset,
        const fastgltf::Primitive & primitive,
        std::string attribute_name,
        std::vector<scene_data::vertex> & vertices,
        T scene_data::vertex::*attribute)
{
	auto it = primitive.findAttribute(attribute_name);

	if (it == primitive.attributes.end())
		return;

	const fastgltf::Accessor & accessor = asset.accessors.at(it->second);

	if (vertices.size() < accessor.count)
		vertices.resize(accessor.count, {});

	fastgltf::iterateAccessorWithIndex<T>(asset, accessor, [&](T value, std::size_t idx) {
		vertices[idx].*attribute = value;
	});
}

namespace
{
constexpr bool starts_with(std::span<const std::byte> data, std::span<const uint8_t> prefix)
{
	return data.size() >= prefix.size() && !memcmp(data.data(), prefix.data(), prefix.size());
}

static fastgltf::MimeType guess_mime_type(std::span<const std::byte> image_data)
{
	const uint8_t png_magic[] = {0xFF, 0xD8, 0xFF};
	const uint8_t jpeg_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

	if (starts_with(image_data, png_magic))
		return fastgltf::MimeType::PNG;
	else if (starts_with(image_data, jpeg_magic))
		return fastgltf::MimeType::JPEG;
	else
		return fastgltf::MimeType::None;
}

static std::pair<scene_data::image, buffer_allocation> do_load_image(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cb,
        fastgltf::MimeType image_type,
        std::span<const std::byte> image_data,
        bool srgb)
{
	std::pair<scene_data::image, buffer_allocation> output;

	cb.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	if (image_type == fastgltf::MimeType::GltfBuffer || image_type == fastgltf::MimeType::None)
		image_type = guess_mime_type(image_data);

	switch (image_type)
	{
		case fastgltf::MimeType::JPEG:
		case fastgltf::MimeType::PNG: {
			image_loader loader(device, cb, image_data, srgb);
			output.first.image_view = std::move(loader.image_view);
			output.first.image_ = std::move(loader.image);
			output.second = std::move(loader.staging_buffer);

			spdlog::debug("Loaded image {}x{}, format {}, {} mipmaps", loader.extent.width, loader.extent.height, vk::to_string(loader.format), loader.num_mipmaps);
		}
		break;

		case fastgltf::MimeType::KTX2:
			// TODO

		case fastgltf::MimeType::DDS:
			// TODO

		default:
			throw std::runtime_error("Invalid image MIME type: " + std::to_string((int)image_type));
	}

	cb.end();

	return output;
}

class loader_context
{
	std::filesystem::path base_directory;
	fastgltf::Asset & gltf;
	vk::raii::Device & device;
	vk::raii::Queue & queue;
	vk::raii::CommandPool & cb_pool;

	std::vector<asset> loaded_assets;
	asset & load_from_asset(const std::filesystem::path & path)
	{
		return loaded_assets.emplace_back(path);
	}

public:
	loader_context(std::filesystem::path base_directory,
	               fastgltf::Asset & gltf,
	               vk::raii::Device & device,
	               vk::raii::Queue & queue,
	               vk::raii::CommandPool & cb_pool) :
	        base_directory(base_directory),
	        gltf(gltf),
	        device(device),
	        queue(queue),
	        cb_pool(cb_pool)
	{
	}

	fastgltf::span<const std::byte> load(const fastgltf::URI& uri)
	{
		auto path = base_directory.empty() ? std::filesystem::path(uri.path()) : base_directory / std::filesystem::path(uri.path());
		std::span<const std::byte> bytes;

		bytes = load_from_asset(path);

		return fastgltf::span<const std::byte>(bytes.data(), bytes.size());
	}

	fastgltf::span<const std::byte> load(const fastgltf::sources::URI& uri)
	{
		return load(uri.uri);
	}

	template<typename T, std::size_t Extent>
	fastgltf::span<T, fastgltf::dynamic_extent> subspan(fastgltf::span<T, Extent> span, size_t offset, size_t count = fastgltf::dynamic_extent)
	{
		assert(offset < span.size());
		assert(count == fastgltf::dynamic_extent || count <= span.size() - offset);

		if (count == fastgltf::dynamic_extent)
			count = span.size() - offset;

		return fastgltf::span<T>{span.data() + offset, count};
	}

	fastgltf::sources::ByteView visit_source(fastgltf::DataSource & source)
	{
		using return_type = fastgltf::sources::ByteView;

		return std::visit(fastgltf::visitor{
		                          [&](std::monostate) -> return_type {
			                          throw std::runtime_error("Invalid source");
		                          },
		                          [&](fastgltf::sources::Fallback) -> return_type {
			                          throw std::runtime_error("Invalid source");
		                          },
		                          [&](const fastgltf::sources::BufferView & buffer_view) -> return_type {
			                          fastgltf::BufferView & view = gltf.bufferViews.at(buffer_view.bufferViewIndex);

			                          fastgltf::Buffer & buffer = gltf.buffers.at(view.bufferIndex);

						  auto buffer2 = visit_source(buffer.data);

			                          return {subspan(buffer2.bytes, view.byteOffset, view.byteLength), buffer_view.mimeType};
		                          },
		                          [&](const fastgltf::sources::URI & uri) -> return_type {
			                          if (!uri.uri.isLocalPath())
				                          throw std::runtime_error("Non local paths are not supported"); // TODO ?

						  fastgltf::span<const std::byte> data = load(uri);

			                          // Don't use the MIME type from fastgltf, it's not initialized for URIs
			                          return {subspan(data, uri.fileByteOffset), fastgltf::MimeType::None};
		                          },
		                          [&](const fastgltf::sources::Vector & vector) -> return_type {
			                          fastgltf::span<const std::byte> data{reinterpret_cast<const std::byte *>(vector.bytes.data()), vector.bytes.size()};
			                          return {data, vector.mimeType};
		                          },
		                          [&](const fastgltf::sources::CustomBuffer & custom_buffer) -> return_type {
			                          throw std::runtime_error("Unimplemented source CustomBuffer");
			                          // TODO ?
		                          },
		                          [&](fastgltf::sources::ByteView & byte_view) -> return_type {
			                          return {byte_view.bytes, byte_view.mimeType};
		                          }},
		                  source);
	}

	std::vector<std::shared_ptr<scene_data::image>> load_all_images()
	{
		// Determine which image is sRGB
		std::vector<bool> srgb;
		srgb.resize(gltf.images.size(), false);
		for (const fastgltf::Material & gltf_material: gltf.materials)
		{
			if (gltf_material.pbrData.baseColorTexture)
				srgb.at(gltf_material.pbrData.baseColorTexture->textureIndex) = true;

			if (gltf_material.emissiveTexture)
				srgb.at(gltf_material.emissiveTexture->textureIndex) = true;
		}

		// Load images
		std::vector<std::shared_ptr<scene_data::image>> images;
		images.reserve(gltf.images.size());

		// Create command buffers and fences
		int num_images_in_flight = 3;
		std::vector<vk::raii::CommandBuffer> command_buffers = device.allocateCommandBuffers({
		        .commandPool = *cb_pool,
		        .level = vk::CommandBufferLevel::ePrimary,
		        .commandBufferCount = (uint32_t)num_images_in_flight,
		});

		std::vector<std::pair<vk::raii::Fence, buffer_allocation>> fences;
		for (int i = 0; i < num_images_in_flight; ++i)
			fences.emplace_back(
			        device.createFence(vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled}),
			        buffer_allocation{});

		// TODO: use a single command buffer?
		for (const auto [index, gltf_image]: utils::enumerate(gltf.images))
		{
			auto & cb = command_buffers[index % num_images_in_flight];
			auto & fence = fences[index % num_images_in_flight].first;

			// Wait for the previous submit to finish before overwriting fences[...].second
			device.waitForFences(*fence, true, 1'000'000'000); // TODO check for timeout

			auto [image_data, mime_type] = visit_source(gltf_image.data);
			auto [image, resources] = do_load_image(device, cb, mime_type, image_data, srgb[index]);


			device.resetFences(*fence);
			fences[index % num_images_in_flight].second = std::move(resources);
			images.emplace_back(std::make_shared<scene_data::image>(std::move(image)));

			vk::SubmitInfo info;
			info.setCommandBuffers(*cb);

			queue.submit(info, *fence);
		}

		std::vector<vk::Fence> fences2;
		for (auto & i: fences)
			fences2.push_back(*i.first);

		device.waitForFences(fences2, true, 1'000'000'000); // TODO check for timeout
		fences.clear();

		return images;
	}

	std::vector<std::shared_ptr<scene_data::texture>> load_all_textures(std::vector<std::shared_ptr<scene_data::image>> & images)
	{
		std::vector<std::shared_ptr<scene_data::texture>> textures;
		textures.reserve(gltf.textures.size());
		for (const fastgltf::Texture & gltf_texture: gltf.textures)
		{
			auto & texture_ref = *textures.emplace_back(std::make_shared<scene_data::texture>());

			if (gltf_texture.samplerIndex)
			{
				fastgltf::Sampler & sampler = gltf.samplers.at(*gltf_texture.samplerIndex);
				texture_ref.sampler = convert(sampler);
			}

			if (gltf_texture.imageIndex)
			{
				std::shared_ptr<scene_data::image> image = images.at(*gltf_texture.imageIndex);

				// Use the aliasing constructor so that the image_view has the same lifetime as the image
				texture_ref.image_view = std::shared_ptr<vk::raii::ImageView>(image, &image->image_view);
			}
			// else if (gltf_texture.basisuImageIndex)
			// {
			// 	// TODO
			// }
			// else if (gltf_texture.ddsImageIndex)
			// {
			// 	// TODO
			// }
			// else if (gltf_texture.webpImageIndex)
			// {
			// 	// TODO
			// }
			else
			{
				throw std::runtime_error("Unsupported image type");
			}
		}

		return textures;
	}

	void load_all_buffers()
	{
		for (fastgltf::Buffer & buffer: gltf.buffers)
		{
			if (std::holds_alternative<fastgltf::sources::URI>(buffer.data))
			{
				fastgltf::sources::URI uri = std::get<fastgltf::sources::URI>(buffer.data);

				// Don't use the MIME type from fastgltf, it's not initialized for URIs
				buffer.data = fastgltf::sources::ByteView(load(uri), fastgltf::MimeType::None);
			}
		}
	}

	std::vector<std::shared_ptr<scene_data::material>> load_all_materials(std::vector<std::shared_ptr<scene_data::texture>> & textures, gpu_buffer & staging_buffer, const scene_data::material & default_material)
	{
		std::vector<std::shared_ptr<scene_data::material>> materials;
		materials.reserve(gltf.materials.size());
		for (const fastgltf::Material & gltf_material: gltf.materials)
		{
			// Copy the default material
			auto & material_ref = *materials.emplace_back(std::make_shared<scene_data::material>(default_material));

			scene_data::material::gpu_data & material_data = material_ref.staging;

			material_data.base_color_factor = convert(gltf_material.pbrData.baseColorFactor);
			material_data.base_emissive_factor = glm::vec4(convert(gltf_material.emissiveFactor), 0);
			material_data.metallic_factor = gltf_material.pbrData.metallicFactor;
			material_data.roughness_factor = gltf_material.pbrData.roughnessFactor;

			if (gltf_material.pbrData.baseColorTexture)
			{
				material_ref.base_color_texture = textures.at(gltf_material.pbrData.baseColorTexture->textureIndex);
				material_data.base_color_texcoord = gltf_material.pbrData.baseColorTexture->texCoordIndex;
			}

			if (gltf_material.pbrData.metallicRoughnessTexture)
			{
				material_ref.metallic_roughness_texture = textures.at(gltf_material.pbrData.metallicRoughnessTexture->textureIndex);
				material_data.metallic_roughness_texcoord = gltf_material.pbrData.metallicRoughnessTexture->texCoordIndex;
			}

			if (gltf_material.occlusionTexture)
			{
				material_ref.occlusion_texture = textures.at(gltf_material.occlusionTexture->textureIndex);
				material_data.occlusion_texcoord = gltf_material.occlusionTexture->texCoordIndex;
				material_data.occlusion_strength = gltf_material.occlusionTexture->strength;
			}

			if (gltf_material.emissiveTexture)
			{
				material_ref.emissive_texture = textures.at(gltf_material.emissiveTexture->textureIndex);
				material_data.emissive_texcoord = gltf_material.emissiveTexture->texCoordIndex;
			}

			if (gltf_material.normalTexture)
			{
				material_ref.normal_texture = textures.at(gltf_material.normalTexture->textureIndex);
				material_data.normal_texcoord = gltf_material.normalTexture->texCoordIndex;
				material_data.normal_scale = gltf_material.normalTexture->scale;
			}

			material_ref.offset = staging_buffer.add_uniform(material_data);
		}

		return materials;
	}

	std::vector<scene_data::mesh> load_all_meshes(std::vector<std::shared_ptr<scene_data::material>> & materials, gpu_buffer & staging_buffer)
	{
		std::vector<scene_data::mesh> meshes;
		meshes.reserve(gltf.meshes.size());
		for (const fastgltf::Mesh & gltf_mesh: gltf.meshes)
		{
			auto & mesh_ref = meshes.emplace_back();

			mesh_ref.primitives.reserve(gltf_mesh.primitives.size());

			for (const fastgltf::Primitive & gltf_primitive: gltf_mesh.primitives)
			{
				auto & primitive_ref = mesh_ref.primitives.emplace_back();

				if (gltf_primitive.indicesAccessor)
				{
					fastgltf::Accessor & indices_accessor = gltf.accessors.at(*gltf_primitive.indicesAccessor);

					primitive_ref.indexed = true;
					primitive_ref.index_offset = staging_buffer.add_indices(indices_accessor);
					primitive_ref.index_count = indices_accessor.count;

					switch (indices_accessor.componentType)
					{
						case fastgltf::ComponentType::Byte:
						case fastgltf::ComponentType::UnsignedByte:
							primitive_ref.index_type = vk::IndexType::eUint8EXT;
							break;

						case fastgltf::ComponentType::Short:
						case fastgltf::ComponentType::UnsignedShort:
							primitive_ref.index_type = vk::IndexType::eUint16;
							break;

						case fastgltf::ComponentType::Int:
						case fastgltf::ComponentType::UnsignedInt:
							primitive_ref.index_type = vk::IndexType::eUint32;
							break;

						default:
							throw std::runtime_error("Invalid index type");
							break;
					}
				}
				else
				{
					primitive_ref.indexed = false;
				}

				std::vector<scene_data::vertex> vertices;

				copy_vertex_attributes(gltf, gltf_primitive, "POSITION", vertices, &scene_data::vertex::position);
				copy_vertex_attributes(gltf, gltf_primitive, "NORMAL", vertices, &scene_data::vertex::normal);
				copy_vertex_attributes(gltf, gltf_primitive, "TANGENT", vertices, &scene_data::vertex::tangent);
				copy_vertex_attributes(gltf, gltf_primitive, "TEXCOORD_", vertices, &scene_data::vertex::texcoord);
				copy_vertex_attributes(gltf, gltf_primitive, "COLOR", vertices, &scene_data::vertex::color);
				copy_vertex_attributes(gltf, gltf_primitive, "JOINTS_", vertices, &scene_data::vertex::joints);
				copy_vertex_attributes(gltf, gltf_primitive, "WEIGHTS_", vertices, &scene_data::vertex::weights);

				primitive_ref.vertex_offset = staging_buffer.add_vertices(vertices);
				primitive_ref.vertex_count = vertices.size();

				primitive_ref.cull_mode = vk::CullModeFlagBits::eBack; // TBC
				primitive_ref.front_face = vk::FrontFace::eClockwise;  // TBC
				primitive_ref.topology = convert(gltf_primitive.type);

				if (gltf_primitive.materialIndex)
					primitive_ref.material_ = materials.at(*gltf_primitive.materialIndex);
			}
		}

		return meshes;
	}

	std::vector<scene_data::scene_object> load_all_objects()
	{
		std::vector<scene_data::scene_object> unsorted_objects;
		unsorted_objects.resize(gltf.nodes.size(), {.parent_id = scene_data::scene_object::root_id });

		for (const auto & [index, gltf_node]: utils::enumerate(gltf.nodes))
		{
			if (gltf_node.meshIndex)
				unsorted_objects[index].mesh_id = *gltf_node.meshIndex;

			for (size_t child: gltf_node.children)
			{
				unsorted_objects[child].parent_id = index;
			}

			auto TRS = std::get<fastgltf::Node::TRS>(gltf_node.transform);

			unsorted_objects[index].translation = glm::make_vec3(TRS.translation.data());
			unsorted_objects[index].rotation = glm::make_quat(TRS.rotation.data());
			unsorted_objects[index].scale = glm::make_vec3(TRS.scale.data());
			unsorted_objects[index].visible = true;
			unsorted_objects[index].name = gltf_node.name;
		}

		return unsorted_objects;
	}

	std::vector<scene_data::scene_object> topological_sort(const std::vector<scene_data::scene_object> & unsorted_objects)
	{
		std::vector<scene_data::scene_object> sorted_objects;
		std::vector<size_t> new_index;
		std::vector<bool> already_sorted;

		sorted_objects.reserve(unsorted_objects.size());
		already_sorted.resize(unsorted_objects.size(), false);
		new_index.resize(unsorted_objects.size(), scene_data::scene_object::root_id);

		bool loop_detected = true;

		while (sorted_objects.size() < unsorted_objects.size())
		{
			for (size_t i = 0; i < unsorted_objects.size(); i++)
			{
				if (already_sorted[i])
					continue;

				if (unsorted_objects[i].parent_id == scene_data::scene_object::root_id)
				{
					sorted_objects.push_back(unsorted_objects[i]);
					already_sorted[i] = true;
					new_index[i] = sorted_objects.size() - 1;
					loop_detected = false;
				}
				else if (already_sorted[unsorted_objects[i].parent_id])
				{
					sorted_objects.emplace_back(unsorted_objects[i]).parent_id = new_index[unsorted_objects[i].parent_id];
					already_sorted[i] = true;
					new_index[i] = sorted_objects.size() - 1;
					loop_detected = false;
				}
			}

			assert(!loop_detected);
		}

		assert(sorted_objects.size() == unsorted_objects.size());
		for(size_t i = 0; i < sorted_objects.size(); i++)
		{
			assert(sorted_objects[i].parent_id == scene_data::scene_object::root_id || sorted_objects[i].parent_id < i);
		}

		return sorted_objects;
	}
};

} // namespace

scene_data scene_loader::operator()(const std::filesystem::path & gltf_path)
{
	vk::PhysicalDeviceProperties physical_device_properties = physical_device.getProperties();
	vk::raii::CommandPool cb_pool{device, vk::CommandPoolCreateInfo{
	                                              .flags = vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
	                                              .queueFamilyIndex = queue_family_index,
	                                      }};

	scene_data data;

	asset asset_file(gltf_path);
	fastgltf::GltfDataBuffer data_buffer;
	data_buffer.copyBytes(reinterpret_cast<const uint8_t*>(asset_file.data()), asset_file.size());

	fastgltf::Asset asset = load_gltf_asset(data_buffer, gltf_path.parent_path());
	loader_context ctx(gltf_path.parent_path(), asset, device, queue, cb_pool);

#ifndef NDEBUG
	if (auto error = fastgltf::validate(asset); error != fastgltf::Error::None)
		throw std::runtime_error(std::string(fastgltf::getErrorMessage(error)));
#endif

	// Load all buffers from URIs
	ctx.load_all_buffers();

	gpu_buffer staging_buffer(physical_device_properties, asset);

	// Load all images
	auto images = ctx.load_all_images();

	// Load all textures
	auto textures = ctx.load_all_textures(images);

	// Load all materials
	auto materials = ctx.load_all_materials(textures, staging_buffer, *default_material);

	// Load all meshes
	data.meshes = ctx.load_all_meshes(materials, staging_buffer);

	data.scene_objects = ctx.topological_sort(ctx.load_all_objects());

	// Copy the staging buffer to the GPU
	spdlog::debug("Uploading scene data ({} bytes) to GPU memory", staging_buffer.size());
	auto buffer = std::make_shared<buffer_allocation>(staging_buffer.copy_to_gpu());

	for (auto & i: materials)
		i->buffer = buffer;

	for (auto & i: data.meshes)
		i.buffer = buffer;

	return data;
}

scene_data & scene_data::import(scene_data && other, scene_object_handle parent)
{
	assert(parent.scene == this || parent.id == scene_object::root_id);

	size_t mesh_offset = meshes.size();
	size_t scene_objects_offset = scene_objects.size();

	for (auto & i: other.meshes)
		meshes.push_back(std::move(i));

	for (auto i: other.scene_objects)
	{
		assert(!i.mesh_id || *i.mesh_id < other.meshes.size());

		if (i.mesh_id)
			*i.mesh_id += mesh_offset;

		if (i.parent_id == scene_object::root_id)
		{
			i.parent_id = parent.id;
		}
		else
		{
			assert(i.parent_id < other.scene_objects.size());
			i.parent_id += scene_objects_offset;
		}

		scene_objects.push_back(i);
	}

	other.meshes.clear();
	other.scene_objects.clear();

	return *this;
}


scene_data & scene_data::import(scene_data && other)
{
	return import(std::move(other), {});
}

scene_object_handle scene_data::new_node()
{
	size_t id = scene_objects.size();

	scene_objects.push_back(scene_object{
		.parent_id = scene_object::root_id,
		.translation = {0, 0, 0},
		.rotation = {1, 0, 0, 0},
		.scale = {1, 1, 1},
		.visible = true
	});

	return {id, this};
}

scene_object_handle scene_data::find_node(std::string_view name)
{
	for(auto&& [index, node]: utils::enumerate(scene_objects))
	{
		if (node.name == name)
		{
			return {index, this};
		}
	}

	// TODO custom exception
	throw std::runtime_error("Node " + std::string(name) + " not found");
}

scene_object_handle scene_data::find_node(scene_object_handle root, std::string_view name)
{
	assert(root.id < scene_objects.size());
	assert(root.scene == this);

	std::vector<bool> flag(scene_objects.size(), false);

	flag[root.id] = true;

	for(size_t index = root.id; index < scene_objects.size(); index++)
	{
		size_t parent = scene_objects[index].parent_id;

		if (parent == scene_object::root_id)
			continue;

		if (!flag[parent])
			continue;

		if (scene_objects[index].name == name)
			return {index, this};

		flag[index] = true;
	}

	// TODO custom exception
	throw std::runtime_error("Node " + std::string(name) + " not found");
}