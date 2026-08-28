#pragma once

namespace voxelstore {
class VoxelStore;
}

/// voxelstore 后端一致性套件：同一套测试跑在所有 VoxelStore 实现上。
/// 约定：传入一个空的新 store。
void run_voxel_conformance(voxelstore::VoxelStore& store);
