# Generates the plugin's default runtime material, /AssimpForUnreal/M_AssimpDefault.
#
# Run inside the editor:
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript
#       -script="<plugin>/Scripts/GenerateDefaultMaterial.py"
#
# Why a generated asset rather than one built by hand: the parameter names this material exposes are
# a contract with AssignMaterialSlots in AssimpBlueprintLibrary.cpp. Generating it from a script
# keeps that contract in source control as readable code, and lets the material be regenerated if
# the parameter set changes, instead of being an opaque binary that has to be re-authored by hand.
#
#
# The "Use<X>Texture" switches
# ---------------------------
# A texture parameter must contribute ONLY when the importer actually bound a texture to it. An
# unbound TextureSampleParameter2D still samples its default texture, so wiring every sampler
# straight to its material output means a model supplying no emissive map still gets whatever the
# default texture holds -- white emissive washes the entire model out to a milky pale, which is
# exactly what happened before this was introduced.
#
# The usual answer is a static switch, but a UMaterialInstanceDynamic cannot change static switches
# at runtime: those require a compiled permutation. Scalar parameters CAN be set on a MID, so each
# map gets a Use<X>Texture scalar (default 0) feeding a Lerp between the flat value and the sampled
# value. AssignMaterialSlots sets it to 1 for exactly the maps it bound.
#
# Sampler types and their default textures must also agree on colour space, or material COMPILATION
# fails ("Sampler type is Normal, should be Color") and the engine silently substitutes the default
# material for the whole thing.

import unreal

PACKAGE_PATH = "/AssimpForUnreal"
ASSET_NAME = "M_AssimpDefault"

# Verified sRGB flags: BaseFlattenNormalMap srgb=False TC_NORMALMAP,
# BaseFlattenLinearColor srgb=False TC_DEFAULT, DefaultTexture srgb=True TC_DEFAULT.
TEX_COLOR = "/Engine/EngineResources/DefaultTexture"
TEX_LINEAR = "/Engine/EngineMaterials/BaseFlattenLinearColor"
TEX_NORMAL = "/Engine/EngineMaterials/BaseFlattenNormalMap"

WHITE = unreal.LinearColor(1.0, 1.0, 1.0, 1.0)
BLACK = unreal.LinearColor(0.0, 0.0, 0.0, 1.0)

mel = unreal.MaterialEditingLibrary


def log(message):
    unreal.log("[GenerateDefaultMaterial] {}".format(message))


def load_texture(path):
    texture = unreal.EditorAssetLibrary.load_asset(path)
    if texture is None:
        raise RuntimeError("could not load default texture '{}'".format(path))
    return texture


class Builder(object):
    """Thin helper over MaterialEditingLibrary, to keep the graph code readable."""

    def __init__(self, material):
        self.material = material

    def _place(self, column, row):
        return (-1800 + column * 320, -1400 + row * 190)

    def sampler(self, name, group, sampler_type, default_texture_path, column, row):
        node = mel.create_material_expression(
            self.material, unreal.MaterialExpressionTextureSampleParameter2D, *self._place(column, row))
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("group", group)
        node.set_editor_property("sampler_type", sampler_type)
        node.set_editor_property("texture", load_texture(default_texture_path))
        return node

    def scalar(self, name, group, default, column, row):
        node = mel.create_material_expression(
            self.material, unreal.MaterialExpressionScalarParameter, *self._place(column, row))
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("group", group)
        node.set_editor_property("default_value", default)
        return node

    def vector(self, name, group, default, column, row):
        node = mel.create_material_expression(
            self.material, unreal.MaterialExpressionVectorParameter, *self._place(column, row))
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("group", group)
        node.set_editor_property("default_value", default)
        return node

    def const3(self, colour, column, row):
        node = mel.create_material_expression(
            self.material, unreal.MaterialExpressionConstant3Vector, *self._place(column, row))
        node.set_editor_property("constant", colour)
        return node

    def const(self, value, column, row):
        node = mel.create_material_expression(
            self.material, unreal.MaterialExpressionConstant, *self._place(column, row))
        node.set_editor_property("r", value)
        return node

    def lerp(self, a, a_out, b, b_out, alpha, column, row):
        node = mel.create_material_expression(
            self.material, unreal.MaterialExpressionLinearInterpolate, *self._place(column, row))
        mel.connect_material_expressions(a, a_out, node, "A")
        mel.connect_material_expressions(b, b_out, node, "B")
        mel.connect_material_expressions(alpha, "", node, "Alpha")
        return node

    def multiply(self, a, a_out, b, b_out, column, row):
        node = mel.create_material_expression(
            self.material, unreal.MaterialExpressionMultiply, *self._place(column, row))
        mel.connect_material_expressions(a, a_out, node, "A")
        mel.connect_material_expressions(b, b_out, node, "B")
        return node

    def to_property(self, node, output, prop):
        mel.connect_material_property(node, output, prop)


def make_material():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    full_path = "{}/{}".format(PACKAGE_PATH, ASSET_NAME)

    # Force a registry scan of the plugin's content root before probing.
    #
    # Both does_asset_exist and load_asset resolve through the asset registry, and a fresh commandlet
    # run starts from a cached registry that predates this plugin's Content directory. Without the
    # scan the probe reports "not found" while create_asset simultaneously refuses with "already
    # exists in package" -- and, unattended, it cannot prompt to overwrite.
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([PACKAGE_PATH], force_rescan=True)

    material = unreal.EditorAssetLibrary.load_asset(full_path)

    if material is not None:
        # Rebuild in place rather than delete-then-create: deleting does not always release the
        # package, and create_asset then refuses and cannot prompt when unattended.
        log("rebuilding existing {}".format(full_path))
        mel.delete_all_material_expressions(material)
    else:
        material = asset_tools.create_asset(
            ASSET_NAME, PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew())
        if material is None:
            raise RuntimeError("could not create the material asset at {}".format(full_path))
        log("created {}".format(full_path))

    # Masked, so models relying on alpha cut-outs (canopies, grilles, foliage) work. The mask
    # defaults to fully opaque and is driven by base-colour alpha only when UseOpacityMask is set,
    # so an opaque model cannot be accidentally cut away.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)

    # TwoSided is not exposed on UMaterial through Python in 5.8, so treat it as best-effort.
    try:
        material.set_editor_property("two_sided", True)
    except Exception as error:
        log("could not set two_sided ({}); leaving it at the default".format(error))

    b = Builder(material)

    # --- Base colour -------------------------------------------------------------------------
    # BaseColor(vector) * Lerp(white, texture.rgb, UseBaseColorTexture).
    # With no texture bound the lerp yields white, so the vector passes through unchanged.
    base_tex = b.sampler("BaseColorTexture", "1 - Base Color",
                         unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, TEX_COLOR, 0, 0)
    use_base = b.scalar("UseBaseColorTexture", "1 - Base Color", 0.0, 0, 1)
    base_vec = b.vector("BaseColor", "1 - Base Color", WHITE, 0, 2)
    base_lerp = b.lerp(b.const3(WHITE, 1, 0), "", base_tex, "RGB", use_base, 2, 0)
    base_final = b.multiply(base_vec, "", base_lerp, "", 3, 0)
    b.to_property(base_final, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # --- Normal ------------------------------------------------------------------------------
    # No switch needed: the default texture is a flat normal, which is the correct no-op.
    normal_tex = b.sampler("NormalTexture", "2 - Normal",
                           unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, TEX_NORMAL, 0, 4)
    b.to_property(normal_tex, "RGB", unreal.MaterialProperty.MP_NORMAL)

    # --- Roughness ---------------------------------------------------------------------------
    rough_tex = b.sampler("RoughnessTexture", "3 - Roughness",
                          unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, TEX_LINEAR, 0, 6)
    use_rough = b.scalar("UseRoughnessTexture", "3 - Roughness", 0.0, 0, 7)
    rough_val = b.scalar("Roughness", "3 - Roughness", 0.5, 0, 8)
    b.to_property(b.lerp(rough_val, "", rough_tex, "R", use_rough, 2, 6), "",
                  unreal.MaterialProperty.MP_ROUGHNESS)

    # --- Metallic ----------------------------------------------------------------------------
    metal_tex = b.sampler("MetallicTexture", "4 - Metallic",
                          unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, TEX_LINEAR, 0, 10)
    use_metal = b.scalar("UseMetallicTexture", "4 - Metallic", 0.0, 0, 11)
    metal_val = b.scalar("Metallic", "4 - Metallic", 0.0, 0, 12)
    b.to_property(b.lerp(metal_val, "", metal_tex, "R", use_metal, 2, 10), "",
                  unreal.MaterialProperty.MP_METALLIC)

    # --- Emissive ----------------------------------------------------------------------------
    # EmissiveColor defaults to BLACK, so black * anything = no emission. This is the channel that
    # produced the washed-out look when it was wired straight from an unbound sampler.
    emis_tex = b.sampler("EmissiveTexture", "5 - Emissive",
                         unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, TEX_COLOR, 0, 14)
    use_emis = b.scalar("UseEmissiveTexture", "5 - Emissive", 0.0, 0, 15)
    emis_vec = b.vector("EmissiveColor", "5 - Emissive", BLACK, 0, 16)
    emis_lerp = b.lerp(b.const3(WHITE, 1, 14), "", emis_tex, "RGB", use_emis, 2, 14)
    b.to_property(b.multiply(emis_vec, "", emis_lerp, "", 3, 14), "",
                  unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # --- Ambient occlusion -------------------------------------------------------------------
    ao_tex = b.sampler("OcclusionTexture", "6 - Occlusion",
                       unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, TEX_LINEAR, 0, 18)
    use_ao = b.scalar("UseOcclusionTexture", "6 - Occlusion", 0.0, 0, 19)
    b.to_property(b.lerp(b.const(1.0, 1, 18), "", ao_tex, "R", use_ao, 2, 18), "",
                  unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)

    # --- Opacity mask ------------------------------------------------------------------------
    # Defaults to 1 (fully opaque). Only when UseOpacityMask is set does base-colour alpha cut the
    # surface, so an opaque model whose PNGs carry odd alpha is not eaten away.
    use_mask = b.scalar("UseOpacityMask", "7 - Opacity", 0.0, 0, 21)
    b.to_property(b.lerp(b.const(1.0, 1, 21), "", base_tex, "A", use_mask, 2, 21), "",
                  unreal.MaterialProperty.MP_OPACITY_MASK)

    # Exposed for callers who make a translucent variant; unused while the blend mode is Masked.
    b.scalar("Opacity", "7 - Opacity", 1.0, 0, 22)

    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(full_path)

    log("{} ready".format(full_path))
    return material


if __name__ == "__main__":
    make_material()
