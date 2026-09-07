#include "MeshWeightRemapMenu.h"

#include "MeshWeightRemapSubsystem.h"

#include "ContentBrowserMenuContexts.h"
#include "Dialog/SCustomDialog.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "PropertyCustomizationHelpers.h"
#include "Styling/CoreStyle.h"
#include "ToolMenus.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshWeightRemap"

namespace UE::MeshWeightRemap::Private
{
	static const FName MenuOwnerName = TEXT("MeshWeightRemap");

	/**
	 * Pick a leader, measure, then commit.
	 *
	 * Measuring first is not politeness. The remap rebuilds the LOD model, so there is no undo, and the
	 * only cheap way to find out whether the definition of driven is the right one for a given pair of
	 * meshes is to read the numbers before anything is written.
	 */
	class SMeshWeightRemapDialog : public SCompoundWidget
	{
	public:

		SLATE_BEGIN_ARGS(SMeshWeightRemapDialog) {}
			SLATE_ARGUMENT(TArray<TWeakObjectPtr<USkeletalMesh>>, Targets)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Targets = InArgs._Targets;

			ChildSlot
			[
				SNew(SBox)
				.MinDesiredWidth(620.f)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 0.f, 0.f, 8.f)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text(FText::Format(LOCTEXT("RemapExplanation",
							"Moves weight off every bone the leader will not evaluate, onto the nearest ancestor "
							"it does. Weights on bones the leader drives are left exactly as they are.\n\n"
							"{0} selected. They are edited in place and there is no undo, so measure first and "
							"save only once the numbers look right."),
							FText::Format(LOCTEXT("MeshCount", "{0} {0}|plural(one=mesh,other=meshes)"),
								Targets.Num())))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 0.f, 0.f, 8.f)
					[
						SNew(SObjectPropertyEntryBox)
						.AllowedClass(USkeletalMesh::StaticClass())
						.AllowClear(true)
						.DisplayThumbnail(true)
						.ObjectPath(this, &SMeshWeightRemapDialog::GetLeaderPath)
						.OnObjectChanged(this, &SMeshWeightRemapDialog::OnLeaderChanged)
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 0.f, 0.f, 8.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Measure", "Measure (changes nothing)"))
						.IsEnabled(this, &SMeshWeightRemapDialog::HasLeader)
						.OnClicked(this, &SMeshWeightRemapDialog::OnMeasureClicked)
					]

					+ SVerticalBox::Slot()
					.FillHeight(1.f)
					[
						SNew(SBox)
						.MinDesiredHeight(180.f)
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
								.Text(this, &SMeshWeightRemapDialog::GetResultText)
							]
						]
					]
				]
			];
		}

		bool HasLeader() const { return Leader.IsValid(); }

		/** Runs for real. Returns the same text the measure pass produces, with the after-numbers filled in. */
		void Commit()
		{
			Run(/*bDryRun*/ false);
		}

	private:

		FString GetLeaderPath() const
		{
			return Leader.IsValid() ? Leader->GetPathName() : FString();
		}

		void OnLeaderChanged(const FAssetData& AssetData)
		{
			Leader = Cast<USkeletalMesh>(AssetData.GetAsset());
			ResultText = FText::GetEmpty();
		}

		FText GetResultText() const { return ResultText; }

		FReply OnMeasureClicked()
		{
			Run(/*bDryRun*/ true);
			return FReply::Handled();
		}

		void Run(bool bDryRun)
		{
			UMeshWeightRemapSubsystem* Subsystem =
				GEditor ? GEditor->GetEditorSubsystem<UMeshWeightRemapSubsystem>() : nullptr;

			if (!Subsystem || !Leader.IsValid())
			{
				ResultText = LOCTEXT("NoLeader", "Pick a leader mesh first.");
				return;
			}

			FString Text;
			for (const TWeakObjectPtr<USkeletalMesh>& Target : Targets)
			{
				if (!Target.IsValid())
				{
					continue;
				}

				const FMeshWeightRemapReport Report = Subsystem->RemapMeshToLeader(Target.Get(), Leader.Get(),
					EMeshWeightRemapDrivenSet::RequiredBones, /*LODIndex*/ 0, bDryRun);

				Text += UMeshWeightRemapSubsystem::DescribeReport(Report);
				Text += TEXT("\n");
			}

			ResultText = FText::FromString(Text);
		}

		TArray<TWeakObjectPtr<USkeletalMesh>> Targets;
		TWeakObjectPtr<USkeletalMesh> Leader;
		FText ResultText;
	};

	static void OpenDialog(const TArray<USkeletalMesh*>& Targets)
	{
		TArray<TWeakObjectPtr<USkeletalMesh>> WeakTargets;
		for (USkeletalMesh* Target : Targets)
		{
			WeakTargets.Add(Target);
		}

		TSharedRef<SMeshWeightRemapDialog> Body = SNew(SMeshWeightRemapDialog).Targets(WeakTargets);

		TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
			.Title(LOCTEXT("DialogTitle", "Remap Weights to Leader"))
			.Content()
			[
				Body
			]
			.Buttons({
				SCustomDialog::FButton(LOCTEXT("Remap", "Remap"))
					.SetPrimary(true)
					.SetButtonRole(SCustomDialog::EButtonRole::Confirm)
					.SetIsEnabled(TAttribute<bool>::CreateSP(&Body.Get(), &SMeshWeightRemapDialog::HasLeader))
					.SetToolTipText(LOCTEXT("RemapTooltip",
						"Writes the change into the selected assets. There is no undo; the packages are left "
						"dirty so the result can still be discarded by not saving.")),
				SCustomDialog::FButton(LOCTEXT("Cancel", "Cancel"))
					.SetButtonRole(SCustomDialog::EButtonRole::Cancel)
			});

		if (Dialog->ShowModal() == 0)
		{
			Body->Commit();
		}
	}
}

void FMeshWeightRemapMenu::Register()
{
	using namespace UE::MeshWeightRemap::Private;

	if (!UToolMenus::IsToolMenuUIEnabled())
	{
		return;
	}

	// Scoped so Unregister can take the entry back out by name on shutdown.
	FToolMenuOwnerScoped OwnerScoped(MenuOwnerName);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu.SkeletalMesh"));
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("GetAssetActions"));
	Section.AddDynamicEntry(TEXT("RemapWeightsToLeader"), FNewToolMenuSectionDelegate::CreateLambda(
		[](FToolMenuSection& InSection)
		{
			UContentBrowserAssetContextMenuContext* Context =
				InSection.FindContext<UContentBrowserAssetContextMenuContext>();
			if (!Context || !Context->bCanBeModified)
			{
				return;
			}

			InSection.AddMenuEntry(
				TEXT("RemapWeightsToLeader"),
				LOCTEXT("MenuEntry", "Remap Weights to Leader..."),
				LOCTEXT("MenuEntryTooltip",
					"Move skin weights off every bone a chosen leader mesh will not drive, so this mesh can be "
					"leader-posed off it without vertices falling back to the reference pose."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([Context]()
				{
					// Deliberately loaded only now - right-clicking an asset should not load it.
					OpenDialog(Context->LoadSelectedObjects<USkeletalMesh>());
				})));
		}));
}

void FMeshWeightRemapMenu::Unregister()
{
	using namespace UE::MeshWeightRemap::Private;

	if (UObjectInitialized() && UToolMenus::TryGet())
	{
		UToolMenus::Get()->UnregisterOwnerByName(MenuOwnerName);
	}
}

#undef LOCTEXT_NAMESPACE
