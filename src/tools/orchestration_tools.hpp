// orchestration_tools.hpp - tools the model needs to run a multi-step task
//
// WHY THESE ARE TOOLS AND NOT PROMPT TEXT
// The context tells the model WHEN to plan / delegate / run a pipeline; a model
// can only act on that advice if the corresponding call exists. Before this
// file the product had a PlanStore, a SubagentManager and a WorkflowStore that
// nothing in the tool list could reach - the prompt was describing features the
// model had no hands for.
//
// The tools are thin: they forward to host callbacks (ToolContext) so the tool
// layer stays free of store ownership and remains testable with stubs.
#pragma once

#include "tool_registry.hpp"

namespace fs {

// plan_update / plan_get
// subagent_spawn / subagent_list / subagent_cancel / subagent_steer
// workflow_list / workflow_start
void register_orchestration_tools(ToolRegistry* registry);

}  // namespace fs
