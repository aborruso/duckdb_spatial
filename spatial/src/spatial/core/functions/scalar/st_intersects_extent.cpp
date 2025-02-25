#include "spatial/common.hpp"
#include "spatial/core/types.hpp"
#include "spatial/core/functions/scalar.hpp"
#include "spatial/core/functions/common.hpp"

#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace spatial {

namespace core {

static void IntersectsExtentFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &left = args.data[0];
	auto &right = args.data[1];
	auto count = args.size();

	BinaryExecutor::Execute<string_t, string_t, bool>(left, right, result, count, [&](string_t left, string_t right) {
		BoundingBox left_bbox;
		BoundingBox right_bbox;
		if (GeometryFactory::TryGetSerializedBoundingBox(left, left_bbox) &&
		    GeometryFactory::TryGetSerializedBoundingBox(right, right_bbox)) {
			return left_bbox.Intersects(right_bbox);
		}
		return false;
	});
}

void CoreScalarFunctions::RegisterStIntersectsExtent(DatabaseInstance &db) {
	ScalarFunction intersects_func("ST_Intersects_Extent", {GeoTypes::GEOMETRY(), GeoTypes::GEOMETRY()},
	                               LogicalType::BOOLEAN, IntersectsExtentFunction);

	ExtensionUtil::RegisterFunction(db, intersects_func);

	// So because this is a macro, we cant overload it. crap.
	// Provide a "&&" macro
	/*
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(make_uniq<ColumnRefExpression>("left"));
	args.push_back(make_uniq<ColumnRefExpression>("right"));

	auto func = make_uniq_base<ParsedExpression, FunctionExpression>("st_intersects_extent", std::move(args));
	auto macro = make_uniq_base<MacroFunction, ScalarMacroFunction>(std::move(func));

	vector<unique_ptr<ParsedExpression>> macro_args;
	macro_args.push_back(make_uniq<ColumnRefExpression>("left"));
	macro_args.push_back(make_uniq<ColumnRefExpression>("right"));
	macro->parameters = std::move(macro_args);

	CreateMacroInfo macro_info(CatalogType::MACRO_ENTRY);
	macro_info.name = "&&";
	macro_info.function = std::move(macro);
	macro_info.schema = DEFAULT_SCHEMA;
	macro_info.internal = true;
	macro_info.parameter_names = {"left", "right"};
	macro_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	ExtensionUtil::RegisterFunction(db, macro_info);
	*/
}

} // namespace core

} // namespace spatial
