// The right-click entry on a skeletal mesh, and the one dialogue behind it.

#pragma once

#include "CoreMinimal.h"

/** Adds "Remap Weights to Leader" to the content browser's skeletal mesh menu. */
class FMeshWeightRemapMenu
{
public:

	static void Register();
	static void Unregister();
};
