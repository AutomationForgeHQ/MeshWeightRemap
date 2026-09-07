using UnrealBuildTool;

public class MeshWeightRemap : ModuleRules
{
	public MeshWeightRemap(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"EditorSubsystem",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"AnimationCore",     // InvMaxRawBoneWeightFloat - raw influence weights are uint16, not floats
				"MeshBoneReduction", // IMeshBoneReduction::ReduceBoneCounts - the redistribution itself
				"PhysicsCore",       // UPhysicsAsset, for the driven-set variant that counts its bodies

				// The right-click entry and its one-field dialogue.
				"Slate",
				"SlateCore",
				"ToolMenus",
				"ToolWidgets",       // SCustomDialog
				"PropertyEditor",    // SObjectPropertyEntryBox
				"ContentBrowser",    // UContentBrowserAssetContextMenuContext
				"InputCore",
			}
			);
	}
}
