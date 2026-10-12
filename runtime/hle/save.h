#pragma once
#include "savefs.h"
#include "ppc_rt.h"
namespace uc { void save_hle_init(SaveFs& files); void save_hle_shutdown(); }
void hle_NANDInit(ppc::Ctx&);
void hle_ISFS_OpenLib(ppc::Ctx&);
void hle_ISFS_CreateDir(ppc::Ctx&); void hle_ISFS_CreateDirAsync(ppc::Ctx&);
void hle_ISFS_CreateFile(ppc::Ctx&); void hle_ISFS_CreateFileAsync(ppc::Ctx&);
void hle_ISFS_Open(ppc::Ctx&); void hle_ISFS_OpenAsync(ppc::Ctx&);
void hle_ISFS_Close(ppc::Ctx&); void hle_ISFS_CloseAsync(ppc::Ctx&);
void hle_ISFS_Read(ppc::Ctx&); void hle_ISFS_ReadAsync(ppc::Ctx&);
void hle_ISFS_Write(ppc::Ctx&); void hle_ISFS_WriteAsync(ppc::Ctx&);
void hle_ISFS_Seek(ppc::Ctx&); void hle_ISFS_SeekAsync(ppc::Ctx&);
void hle_ISFS_GetFileStats(ppc::Ctx&);
void hle_ISFS_GetAttr(ppc::Ctx&); void hle_ISFS_GetAttrAsync(ppc::Ctx&);
void hle_ISFS_GetUsage(ppc::Ctx&); void hle_ISFS_GetUsageAsync(ppc::Ctx&);
void hle_ISFS_ReadDir(ppc::Ctx&); void hle_ISFS_ReadDirAsync(ppc::Ctx&);
void hle_ISFS_Rename(ppc::Ctx&); void hle_ISFS_RenameAsync(ppc::Ctx&);
void hle_ISFS_Delete(ppc::Ctx&); void hle_ISFS_DeleteAsync(ppc::Ctx&);
