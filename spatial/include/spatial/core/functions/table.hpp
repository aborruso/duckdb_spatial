#pragma once
#include "spatial/common.hpp"

namespace spatial {

namespace core {

struct CoreTableFunctions {
public:
	static void Register(DatabaseInstance &db) {
		RegisterOsmTableFunction(db);

		// TODO: Move these
		RegisterShapefileTableFunction(db);
		RegisterShapefileMetaTableFunction(db);
	}

private:
	static void RegisterOsmTableFunction(DatabaseInstance &db);
	static void RegisterShapefileTableFunction(DatabaseInstance &db);
	static void RegisterShapefileMetaTableFunction(DatabaseInstance &db);
};

} // namespace core

} // namespace spatial