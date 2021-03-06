// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/download/drag_download_item.h"

#include <string>

#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/download_item.h"
#include "net/base/mime_util.h"
#include "net/base/net_util.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/dragdrop/drag_utils.h"
#include "ui/base/dragdrop/file_info.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/point.h"
#include "ui/gfx/screen.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

#if defined(USE_AURA)
#include "ui/aura/window.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/wm/public/drag_drop_client.h"
#endif

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/drive/download_handler.h"
#endif

void DragDownloadItem(const content::DownloadItem* download,
                      gfx::Image* icon,
                      gfx::NativeView view) {
  DCHECK(download);
  DCHECK_EQ(content::DownloadItem::COMPLETE, download->GetState());

  // Set up our OLE machinery
  ui::OSExchangeData data;

  drag_utils::CreateDragImageForFile(
      download->GetFileNameToReportUser(),
      icon ? icon->AsImageSkia() : gfx::ImageSkia(),
      &data);

  base::FilePath full_path = download->GetTargetFilePath();
#if defined(OS_CHROMEOS)
  // Overwrite |full_path| with drive cache file path when appropriate.
  Profile* profile = Profile::FromBrowserContext(download->GetBrowserContext());
  drive::DownloadHandler* drive_download_handler =
      drive::DownloadHandler::GetForProfile(profile);
  if (drive_download_handler &&
      drive_download_handler->IsDriveDownload(download))
    full_path = drive_download_handler->GetCacheFilePath(download);
#endif
  std::vector<ui::FileInfo> file_infos;
  file_infos.push_back(
      ui::FileInfo(full_path, download->GetFileNameToReportUser()));
  data.SetFilenames(file_infos);

  // Add URL so that we can load supported files when dragged to WebContents.
  data.SetURL(net::FilePathToFileURL(full_path),
              download->GetFileNameToReportUser().LossyDisplayName());

#if !defined(TOOLKIT_GTK)
#if defined(USE_AURA)
  aura::Window* root_window = view->GetRootWindow();
  if (!root_window || !aura::client::GetDragDropClient(root_window))
    return;

  gfx::Point location = gfx::Screen::GetScreenFor(view)->GetCursorScreenPoint();
  // TODO(varunjain): Properly determine and send DRAG_EVENT_SOURCE below.
  aura::client::GetDragDropClient(root_window)->StartDragAndDrop(
      data,
      root_window,
      view,
      location,
      ui::DragDropTypes::DRAG_COPY | ui::DragDropTypes::DRAG_LINK,
      ui::DragDropTypes::DRAG_EVENT_SOURCE_MOUSE);
#else  // We are on WIN without AURA
  // We cannot use Widget::RunShellDrag on WIN since the |view| is backed by a
  // WebContentsViewWin, not a NativeWidgetWin.
  scoped_refptr<ui::DragSourceWin> drag_source(new ui::DragSourceWin);
  // Run the drag and drop loop
  DWORD effects;
  DoDragDrop(ui::OSExchangeDataProviderWin::GetIDataObject(data),
             drag_source.get(),
             DROPEFFECT_COPY | DROPEFFECT_LINK,
             &effects);
#endif

#else
  GtkWidget* root = gtk_widget_get_toplevel(view);
  if (!root)
    return;

  views::NativeWidgetGtk* widget = static_cast<views::NativeWidgetGtk*>(
      views::Widget::GetWidgetForNativeView(root)->native_widget());
  if (!widget)
    return;

  widget->DoDrag(data,
                 ui::DragDropTypes::DRAG_COPY | ui::DragDropTypes::DRAG_LINK);
#endif  // TOOLKIT_GTK
}
