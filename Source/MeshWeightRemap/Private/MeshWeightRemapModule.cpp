#include "MeshWeightRemapMenu.h"

#include "Modules/ModuleManager.h"
#include "ToolMenus.h"

class FMeshWeightRemapModule : public IModuleInterface
{
public:

	virtual void StartupModule() override
	{
		// The content browser's menus are built by then; registering earlier extends nothing.
		StartupHandle = UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMeshWeightRemapModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		if (StartupHandle.IsValid())
		{
			UToolMenus::UnRegisterStartupCallback(StartupHandle);
			StartupHandle.Reset();
		}

		FMeshWeightRemapMenu::Unregister();
	}

private:

	void RegisterMenus()
	{
		FMeshWeightRemapMenu::Register();
	}

	FDelegateHandle StartupHandle;
};

IMPLEMENT_MODULE(FMeshWeightRemapModule, MeshWeightRemap);
