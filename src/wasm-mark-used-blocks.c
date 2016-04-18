/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm-mark-used-blocks.h"

#include <assert.h>
#include <stdio.h>

#include "wasm-allocator.h"
#include "wasm-ast.h"

#define CHECK_RESULT(expr) \
  do {                     \
    if (WASM_FAILED(expr)) \
      return WASM_ERROR;   \
  } while (0)

#define CHECK_ALLOC(e)                                                   \
  do {                                                                   \
    if (WASM_FAILED(e)) {                                                \
      fprintf(stderr, "%s:%d: allocation failed\n", __FILE__, __LINE__); \
      return WASM_ERROR;                                                 \
    }                                                                    \
  } while (0)

typedef struct WasmLabelNode {
  const WasmLabel* label;
  WasmBool used;
} WasmLabelNode;

WASM_DEFINE_VECTOR(label_node, WasmLabelNode);

typedef struct WasmContext {
  WasmAllocator* allocator;
  WasmFunc* current_func;
  WasmLabelNodeVector labels;
} WasmContext;

static WasmResult push_label_node(WasmContext* ctx, const WasmLabel* label) {
  WasmLabelNode node;
  node.label = label;
  node.used = WASM_FALSE;
  CHECK_ALLOC(
      wasm_append_label_node_value(ctx->allocator, &ctx->labels, &node));
  return WASM_OK;
}

static void pop_label_node(WasmContext* ctx) {
  assert(ctx->labels.size > 0);
  ctx->labels.size--;
}

static WasmLabelNode* top_label_node(WasmContext* ctx) {
  assert(ctx->labels.size > 0);
  return &ctx->labels.data[ctx->labels.size - 1];
}

static WasmLabelNode* find_label_node_by_var(WasmContext* ctx, WasmVar* var) {
  if (var->type == WASM_VAR_TYPE_NAME) {
    int i;
    for (i = ctx->labels.size - 1; i >= 0; --i) {
      WasmLabelNode* node = &ctx->labels.data[i];
      if (wasm_string_slices_are_equal(node->label, &var->name))
        return node;
    }
    return NULL;
  } else {
    if (var->index < 0 || (size_t)var->index >= ctx->labels.size)
      return NULL;
    return &ctx->labels.data[ctx->labels.size - 1 - var->index];
  }
}

static WasmResult mark_node_used(WasmContext* ctx, WasmVar* var) {
  WasmLabelNode* node = find_label_node_by_var(ctx, var);
  if (!node)
    return WASM_ERROR;
  node->used = WASM_TRUE;
  return WASM_OK;
}

static WasmResult begin_block_expr(WasmExpr* expr, void* user_data) {
  WasmContext* ctx = user_data;
  CHECK_RESULT(push_label_node(ctx, &expr->block.label));
  return WASM_OK;
}

static WasmResult end_block_expr(WasmExpr* expr, void* user_data) {
  WasmContext* ctx = user_data;
  WasmLabelNode* node = top_label_node(ctx);
  expr->block.used = node->used;
  pop_label_node(ctx);
  return WASM_OK;
}

static WasmResult begin_loop_expr(WasmExpr* expr, void* user_data) {
  WasmContext* ctx = user_data;
  CHECK_RESULT(push_label_node(ctx, &expr->loop.outer));
  CHECK_RESULT(push_label_node(ctx, &expr->loop.inner));
  return WASM_OK;
}

static WasmResult end_loop_expr(WasmExpr* expr, void* user_data) {
  WasmContext* ctx = user_data;
  expr->loop.inner_used = top_label_node(ctx)->used;
  pop_label_node(ctx);
  expr->loop.outer_used = top_label_node(ctx)->used;
  pop_label_node(ctx);
  return WASM_OK;
}

static WasmResult begin_br_expr(WasmExpr* expr, void* user_data) {
  WasmContext* ctx = user_data;
  CHECK_RESULT(mark_node_used(ctx, &expr->br.var));
  return WASM_OK;
}

static WasmResult begin_br_if_expr(WasmExpr* expr, void* user_data) {
  WasmContext* ctx = user_data;
  CHECK_RESULT(mark_node_used(ctx, &expr->br_if.var));
  return WASM_OK;
}

static WasmResult begin_br_table_expr(WasmExpr* expr, void* user_data) {
  WasmContext* ctx = user_data;
  size_t i;
  for (i = 0; i < expr->br_table.targets.size; ++i) {
    WasmVar* var = &expr->br_table.targets.data[i];
    CHECK_RESULT(mark_node_used(ctx, var));
  }
  CHECK_RESULT(mark_node_used(ctx, &expr->br_table.default_target));
  return WASM_OK;
}

WasmResult wasm_mark_used_blocks(WasmAllocator* allocator, WasmScript* script) {
  WasmContext ctx;
  WASM_ZERO_MEMORY(ctx);
  ctx.allocator = allocator;

  WasmExprVisitor visitor;
  WASM_ZERO_MEMORY(visitor);
  visitor.user_data = &ctx;
  visitor.begin_block_expr = &begin_block_expr;
  visitor.end_block_expr = &end_block_expr;
  visitor.begin_loop_expr = &begin_loop_expr;
  visitor.end_loop_expr = &end_loop_expr;
  visitor.begin_br_expr = &begin_br_expr;
  visitor.begin_br_if_expr = &begin_br_if_expr;
  visitor.begin_br_table_expr = &begin_br_table_expr;

  size_t i;
  for (i = 0; i < script->commands.size; ++i) {
    WasmCommand* command = &script->commands.data[i];
    if (command->type != WASM_COMMAND_TYPE_MODULE)
      continue;
    WasmModule* module = &command->module;
    size_t j;
    for (j = 0; j < module->funcs.size; ++j) {
      WasmFunc* func = module->funcs.data[j];
      ctx.current_func = func;
      CHECK_RESULT(wasm_visit_func(func, &visitor));
      ctx.current_func = NULL;
    }
  }

  wasm_destroy_label_node_vector(allocator, &ctx.labels);
  return WASM_OK;
}