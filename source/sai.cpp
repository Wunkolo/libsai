// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

#include <algorithm>
#include <fstream>

namespace sai
{

VirtualFileVisitor::~VirtualFileVisitor()
{
}

bool VirtualFileVisitor::VisitFolderBegin(VirtualFileEntry& /*Entry*/)
{
	return true;
}

bool VirtualFileVisitor::VisitFolderEnd(VirtualFileEntry& /*Entry*/)
{
	return true;
}

bool VirtualFileVisitor::VisitFile(VirtualFileEntry& /*Entry*/)
{
	return true;
}

} // namespace sai
