// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/bookmark_manager_private/bookmark_manager_private_api.h"

#include <vector>

#include "base/lazy_instance.h"
#include "base/memory/linked_ptr.h"
#include "base/prefs/pref_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/bookmarks/bookmark_node_data.h"
#include "chrome/browser/bookmarks/bookmark_stats.h"
#include "chrome/browser/bookmarks/bookmark_utils.h"
#include "chrome/browser/bookmarks/scoped_group_bookmark_actions.h"
#include "chrome/browser/extensions/api/bookmarks/bookmark_api_constants.h"
#include "chrome/browser/extensions/api/bookmarks/bookmark_api_helpers.h"
#include "chrome/browser/extensions/extension_web_ui.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/bookmarks/bookmark_drag_drop.h"
#include "chrome/browser/undo/bookmark_undo_service.h"
#include "chrome/browser/undo/bookmark_undo_service_factory.h"
#include "chrome/common/extensions/api/bookmark_manager_private.h"
#include "chrome/common/pref_names.h"
#include "components/user_prefs/user_prefs.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_view.h"
#include "content/public/browser/web_ui.h"
#include "extensions/browser/extension_function_dispatcher.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/view_type_utils.h"
#include "grit/generated_resources.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/webui/web_ui_util.h"

#if defined(OS_WIN)
#include "win8/util/win8_util.h"
#endif  // OS_WIN

namespace extensions {

namespace bookmark_keys = bookmark_api_constants;
namespace bookmark_manager_private = api::bookmark_manager_private;
namespace CanPaste = api::bookmark_manager_private::CanPaste;
namespace Copy = api::bookmark_manager_private::Copy;
namespace Cut = api::bookmark_manager_private::Cut;
namespace Drop = api::bookmark_manager_private::Drop;
namespace GetSubtree = api::bookmark_manager_private::GetSubtree;
namespace GetMetaInfo = api::bookmark_manager_private::GetMetaInfo;
namespace Paste = api::bookmark_manager_private::Paste;
namespace RedoInfo = api::bookmark_manager_private::GetRedoInfo;
namespace RemoveTrees = api::bookmark_manager_private::RemoveTrees;
namespace SetMetaInfo = api::bookmark_manager_private::SetMetaInfo;
namespace SortChildren = api::bookmark_manager_private::SortChildren;
namespace StartDrag = api::bookmark_manager_private::StartDrag;
namespace UndoInfo = api::bookmark_manager_private::GetUndoInfo;

using content::WebContents;

namespace {

// Returns a single bookmark node from the argument ID.
// This returns NULL in case of failure.
const BookmarkNode* GetNodeFromString(
    BookmarkModel* model, const std::string& id_string) {
  int64 id;
  if (!base::StringToInt64(id_string, &id))
    return NULL;
  return model->GetNodeByID(id);
}

// Gets a vector of bookmark nodes from the argument list of IDs.
// This returns false in the case of failure.
bool GetNodesFromVector(BookmarkModel* model,
                        const std::vector<std::string>& id_strings,
                        std::vector<const BookmarkNode*>* nodes) {

  if (id_strings.empty())
    return false;

  for (size_t i = 0; i < id_strings.size(); ++i) {
    const BookmarkNode* node = GetNodeFromString(model, id_strings[i]);
    if (!node)
      return false;
    nodes->push_back(node);
  }

  return true;
}

// Recursively create a bookmark_manager_private::BookmarkNodeDataElement from
// a bookmark node. This is by used |BookmarkNodeDataToJSON| when the data comes
// from the current profile. In this case we have a BookmarkNode since we got
// the data from the current profile.
linked_ptr<bookmark_manager_private::BookmarkNodeDataElement>
CreateNodeDataElementFromBookmarkNode(const BookmarkNode& node) {
  linked_ptr<bookmark_manager_private::BookmarkNodeDataElement> element(
      new bookmark_manager_private::BookmarkNodeDataElement);
  // Add id and parentId so we can associate the data with existing nodes on the
  // client side.
  element->id.reset(new std::string(base::Int64ToString(node.id())));
  element->parent_id.reset(
      new std::string(base::Int64ToString(node.parent()->id())));

  if (node.is_url())
    element->url.reset(new std::string(node.url().spec()));

  element->title = base::UTF16ToUTF8(node.GetTitle());
  for (int i = 0; i < node.child_count(); ++i) {
    element->children.push_back(
        CreateNodeDataElementFromBookmarkNode(*node.GetChild(i)));
  }

  return element;
}

// Recursively create a bookmark_manager_private::BookmarkNodeDataElement from
// a BookmarkNodeData::Element. This is used by |BookmarkNodeDataToJSON| when
// the data comes from a different profile. When the data comes from a different
// profile we do not have any IDs or parent IDs.
linked_ptr<bookmark_manager_private::BookmarkNodeDataElement>
CreateApiNodeDataElement(const BookmarkNodeData::Element& element) {
  linked_ptr<bookmark_manager_private::BookmarkNodeDataElement> node_element(
      new bookmark_manager_private::BookmarkNodeDataElement);

  if (element.is_url)
    node_element->url.reset(new std::string(element.url.spec()));
  node_element->title = base::UTF16ToUTF8(element.title);
  for (size_t i = 0; i < element.children.size(); ++i) {
    node_element->children.push_back(
        CreateApiNodeDataElement(element.children[i]));
  }

  return node_element;
}

// Creates a bookmark_manager_private::BookmarkNodeData from a BookmarkNodeData.
scoped_ptr<bookmark_manager_private::BookmarkNodeData>
CreateApiBookmarkNodeData(Profile* profile, const BookmarkNodeData& data) {
  scoped_ptr<bookmark_manager_private::BookmarkNodeData> node_data(
      new bookmark_manager_private::BookmarkNodeData);
  node_data->same_profile = data.IsFromProfile(profile);

  if (node_data->same_profile) {
    std::vector<const BookmarkNode*> nodes = data.GetNodes(profile);
    for (size_t i = 0; i < nodes.size(); ++i) {
      node_data->elements.push_back(
          CreateNodeDataElementFromBookmarkNode(*nodes[i]));
    }
  } else {
    // We do not have a node IDs when the data comes from a different profile.
    std::vector<BookmarkNodeData::Element> elements = data.elements;
    for (size_t i = 0; i < elements.size(); ++i)
      node_data->elements.push_back(CreateApiNodeDataElement(elements[i]));
  }
  return node_data.Pass();
}

}  // namespace

BookmarkManagerPrivateEventRouter::BookmarkManagerPrivateEventRouter(
    content::BrowserContext* browser_context,
    BookmarkModel* bookmark_model)
    : browser_context_(browser_context), bookmark_model_(bookmark_model) {
  bookmark_model_->AddObserver(this);
}

BookmarkManagerPrivateEventRouter::~BookmarkManagerPrivateEventRouter() {
  if (bookmark_model_)
    bookmark_model_->RemoveObserver(this);
}

void BookmarkManagerPrivateEventRouter::DispatchEvent(
    const std::string& event_name,
    scoped_ptr<base::ListValue> event_args) {
  extensions::ExtensionSystem::Get(browser_context_)
      ->event_router()
      ->BroadcastEvent(make_scoped_ptr(
          new extensions::Event(event_name, event_args.Pass())));
}

void BookmarkManagerPrivateEventRouter::BookmarkModelChanged() {}

void BookmarkManagerPrivateEventRouter::BookmarkModelBeingDeleted(
    BookmarkModel* model) {
  bookmark_model_ = NULL;
}

void BookmarkManagerPrivateEventRouter::BookmarkMetaInfoChanged(
    BookmarkModel* model,
    const BookmarkNode* node) {
  DispatchEvent(bookmark_manager_private::OnMetaInfoChanged::kEventName,
                bookmark_manager_private::OnMetaInfoChanged::Create(
                    base::Int64ToString(node->id())));
}

BookmarkManagerPrivateAPI::BookmarkManagerPrivateAPI(
    content::BrowserContext* browser_context)
    : browser_context_(browser_context) {
  EventRouter* event_router =
      ExtensionSystem::Get(browser_context)->event_router();
  event_router->RegisterObserver(
      this, bookmark_manager_private::OnMetaInfoChanged::kEventName);
}

BookmarkManagerPrivateAPI::~BookmarkManagerPrivateAPI() {}

void BookmarkManagerPrivateAPI::Shutdown() {
  ExtensionSystem::Get(browser_context_)->event_router()->UnregisterObserver(
      this);
}

static base::LazyInstance<
    BrowserContextKeyedAPIFactory<BookmarkManagerPrivateAPI> > g_factory =
    LAZY_INSTANCE_INITIALIZER;

// static
BrowserContextKeyedAPIFactory<BookmarkManagerPrivateAPI>*
BookmarkManagerPrivateAPI::GetFactoryInstance() {
  return g_factory.Pointer();
}

void BookmarkManagerPrivateAPI::OnListenerAdded(
    const EventListenerInfo& details) {
  ExtensionSystem::Get(browser_context_)->event_router()->UnregisterObserver(
      this);
  event_router_.reset(new BookmarkManagerPrivateEventRouter(
      browser_context_,
      BookmarkModelFactory::GetForProfile(
          Profile::FromBrowserContext(browser_context_))));
}

BookmarkManagerPrivateDragEventRouter::BookmarkManagerPrivateDragEventRouter(
    Profile* profile,
    content::WebContents* web_contents)
    : profile_(profile), web_contents_(web_contents) {
  BookmarkTabHelper* bookmark_tab_helper =
      BookmarkTabHelper::FromWebContents(web_contents_);
  bookmark_tab_helper->set_bookmark_drag_delegate(this);
}

BookmarkManagerPrivateDragEventRouter::
    ~BookmarkManagerPrivateDragEventRouter() {
  BookmarkTabHelper* bookmark_tab_helper =
      BookmarkTabHelper::FromWebContents(web_contents_);
  if (bookmark_tab_helper->bookmark_drag_delegate() == this)
    bookmark_tab_helper->set_bookmark_drag_delegate(NULL);
}

void BookmarkManagerPrivateDragEventRouter::DispatchEvent(
    const std::string& event_name,
    scoped_ptr<base::ListValue> args) {
  if (!ExtensionSystem::Get(profile_)->event_router())
    return;

  scoped_ptr<Event> event(new Event(event_name, args.Pass()));
  ExtensionSystem::Get(profile_)->event_router()->BroadcastEvent(event.Pass());
}

void BookmarkManagerPrivateDragEventRouter::OnDragEnter(
    const BookmarkNodeData& data) {
  if (data.size() == 0)
    return;
  DispatchEvent(bookmark_manager_private::OnDragEnter::kEventName,
                bookmark_manager_private::OnDragEnter::Create(
                    *CreateApiBookmarkNodeData(profile_, data)));
}

void BookmarkManagerPrivateDragEventRouter::OnDragOver(
    const BookmarkNodeData& data) {
  // Intentionally empty since these events happens too often and floods the
  // message queue. We do not need this event for the bookmark manager anyway.
}

void BookmarkManagerPrivateDragEventRouter::OnDragLeave(
    const BookmarkNodeData& data) {
  if (data.size() == 0)
    return;
  DispatchEvent(bookmark_manager_private::OnDragLeave::kEventName,
                bookmark_manager_private::OnDragLeave::Create(
                    *CreateApiBookmarkNodeData(profile_, data)));
}

void BookmarkManagerPrivateDragEventRouter::OnDrop(
    const BookmarkNodeData& data) {
  if (data.size() == 0)
    return;
  DispatchEvent(bookmark_manager_private::OnDrop::kEventName,
                bookmark_manager_private::OnDrop::Create(
                    *CreateApiBookmarkNodeData(profile_, data)));

  // Make a copy that is owned by this instance.
  ClearBookmarkNodeData();
  bookmark_drag_data_ = data;
}

const BookmarkNodeData*
BookmarkManagerPrivateDragEventRouter::GetBookmarkNodeData() {
  if (bookmark_drag_data_.is_valid())
    return &bookmark_drag_data_;
  return NULL;
}

void BookmarkManagerPrivateDragEventRouter::ClearBookmarkNodeData() {
  bookmark_drag_data_.Clear();
}

bool ClipboardBookmarkManagerFunction::CopyOrCut(bool cut,
    const std::vector<std::string>& id_list) {
  BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());
  std::vector<const BookmarkNode*> nodes;
  EXTENSION_FUNCTION_VALIDATE(GetNodesFromVector(model, id_list, &nodes));
  bookmark_utils::CopyToClipboard(model, nodes, cut);
  return true;
}

bool BookmarkManagerPrivateCopyFunction::RunImpl() {
  scoped_ptr<Copy::Params> params(Copy::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);
  return CopyOrCut(false, params->id_list);
}

bool BookmarkManagerPrivateCutFunction::RunImpl() {
  if (!EditBookmarksEnabled())
    return false;

  scoped_ptr<Cut::Params> params(Cut::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);
  return CopyOrCut(true, params->id_list);
}

bool BookmarkManagerPrivatePasteFunction::RunImpl() {
  if (!EditBookmarksEnabled())
    return false;

  scoped_ptr<Paste::Params> params(Paste::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);
  BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());
  const BookmarkNode* parent_node = GetNodeFromString(model, params->parent_id);
  if (!parent_node) {
    error_ = bookmark_keys::kNoParentError;
    return false;
  }
  bool can_paste = bookmark_utils::CanPasteFromClipboard(parent_node);
  if (!can_paste)
    return false;

  // We want to use the highest index of the selected nodes as a destination.
  std::vector<const BookmarkNode*> nodes;
  // No need to test return value, if we got an empty list, we insert at end.
  if (params->selected_id_list)
    GetNodesFromVector(model, *params->selected_id_list, &nodes);
  int highest_index = -1;  // -1 means insert at end of list.
  for (size_t i = 0; i < nodes.size(); ++i) {
    // + 1 so that we insert after the selection.
    int index = parent_node->GetIndexOf(nodes[i]) + 1;
    if (index > highest_index)
      highest_index = index;
  }

  bookmark_utils::PasteFromClipboard(model, parent_node, highest_index);
  return true;
}

bool BookmarkManagerPrivateCanPasteFunction::RunImpl() {
  if (!EditBookmarksEnabled())
    return false;

  scoped_ptr<CanPaste::Params> params(CanPaste::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());
  const BookmarkNode* parent_node = GetNodeFromString(model, params->parent_id);
  if (!parent_node) {
    error_ = bookmark_keys::kNoParentError;
    return false;
  }
  bool can_paste = bookmark_utils::CanPasteFromClipboard(parent_node);
  SetResult(new base::FundamentalValue(can_paste));
  return true;
}

bool BookmarkManagerPrivateSortChildrenFunction::RunImpl() {
  if (!EditBookmarksEnabled())
    return false;

  scoped_ptr<SortChildren::Params> params(SortChildren::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());
  const BookmarkNode* parent_node = GetNodeFromString(model, params->parent_id);
  if (!parent_node) {
    error_ = bookmark_keys::kNoParentError;
    return false;
  }
  model->SortChildren(parent_node);
  return true;
}

bool BookmarkManagerPrivateGetStringsFunction::RunImpl() {
  base::DictionaryValue* localized_strings = new base::DictionaryValue();

  localized_strings->SetString("title",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_TITLE));
  localized_strings->SetString("search_button",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_SEARCH_BUTTON));
  localized_strings->SetString("organize_menu",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_ORGANIZE_MENU));
  localized_strings->SetString("show_in_folder",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_SHOW_IN_FOLDER));
  localized_strings->SetString("sort",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_SORT));
  localized_strings->SetString("import_menu",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_IMPORT_MENU));
  localized_strings->SetString("export_menu",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_EXPORT_MENU));
  localized_strings->SetString("rename_folder",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_RENAME_FOLDER));
  localized_strings->SetString("edit",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_EDIT));
  localized_strings->SetString("should_open_all",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_SHOULD_OPEN_ALL));
  localized_strings->SetString("open_incognito",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_OPEN_INCOGNITO));
  localized_strings->SetString("open_in_new_tab",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_OPEN_IN_NEW_TAB));
  localized_strings->SetString("open_in_new_window",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_OPEN_IN_NEW_WINDOW));
  localized_strings->SetString("add_new_bookmark",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_ADD_NEW_BOOKMARK));
  localized_strings->SetString("new_folder",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_NEW_FOLDER));
  localized_strings->SetString("open_all",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_OPEN_ALL));
  localized_strings->SetString("open_all_new_window",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_OPEN_ALL_NEW_WINDOW));
  localized_strings->SetString("open_all_incognito",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_OPEN_ALL_INCOGNITO));
  localized_strings->SetString("remove",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_BAR_REMOVE));
  localized_strings->SetString("copy",
      l10n_util::GetStringUTF16(IDS_CONTENT_CONTEXT_COPY));
  localized_strings->SetString("cut",
      l10n_util::GetStringUTF16(IDS_CONTENT_CONTEXT_CUT));
  localized_strings->SetString("paste",
      l10n_util::GetStringUTF16(IDS_CONTENT_CONTEXT_PASTE));
  localized_strings->SetString("delete",
      l10n_util::GetStringUTF16(IDS_CONTENT_CONTEXT_DELETE));
  localized_strings->SetString("undo_delete",
      l10n_util::GetStringUTF16(IDS_UNDO_DELETE));
  localized_strings->SetString("new_folder_name",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_EDITOR_NEW_FOLDER_NAME));
  localized_strings->SetString("name_input_placeholder",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_NAME_INPUT_PLACE_HOLDER));
  localized_strings->SetString("url_input_placeholder",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_URL_INPUT_PLACE_HOLDER));
  localized_strings->SetString("invalid_url",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_INVALID_URL));
  localized_strings->SetString("recent",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_RECENT));
  localized_strings->SetString("search",
      l10n_util::GetStringUTF16(IDS_BOOKMARK_MANAGER_SEARCH));
  localized_strings->SetString("save",
      l10n_util::GetStringUTF16(IDS_SAVE));
  localized_strings->SetString("cancel",
      l10n_util::GetStringUTF16(IDS_CANCEL));

  webui::SetFontAndTextDirection(localized_strings);

  SetResult(localized_strings);

  // This is needed because unlike the rest of these functions, this class
  // inherits from AsyncFunction directly, rather than BookmarkFunction.
  SendResponse(true);

  return true;
}

bool BookmarkManagerPrivateStartDragFunction::RunImpl() {
  if (!EditBookmarksEnabled())
    return false;

  scoped_ptr<StartDrag::Params> params(StartDrag::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());
  std::vector<const BookmarkNode*> nodes;
  EXTENSION_FUNCTION_VALIDATE(
      GetNodesFromVector(model, params->id_list, &nodes));

  WebContents* web_contents =
      WebContents::FromRenderViewHost(render_view_host_);
  if (GetViewType(web_contents) == VIEW_TYPE_TAB_CONTENTS) {
    WebContents* web_contents =
        dispatcher()->delegate()->GetAssociatedWebContents();
    CHECK(web_contents);

    ui::DragDropTypes::DragEventSource source =
        ui::DragDropTypes::DRAG_EVENT_SOURCE_MOUSE;
    if (params->is_from_touch)
      source = ui::DragDropTypes::DRAG_EVENT_SOURCE_TOUCH;

    chrome::DragBookmarks(
        GetProfile(), nodes, web_contents->GetView()->GetNativeView(), source);

    return true;
  } else {
    NOTREACHED();
    return false;
  }
}

bool BookmarkManagerPrivateDropFunction::RunImpl() {
  if (!EditBookmarksEnabled())
    return false;

  scoped_ptr<Drop::Params> params(Drop::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());

  const BookmarkNode* drop_parent = GetNodeFromString(model, params->parent_id);
  if (!drop_parent) {
    error_ = bookmark_keys::kNoParentError;
    return false;
  }

  int drop_index;
  if (params->index)
    drop_index = *params->index;
  else
    drop_index = drop_parent->child_count();

  WebContents* web_contents =
      WebContents::FromRenderViewHost(render_view_host_);
  if (GetViewType(web_contents) == VIEW_TYPE_TAB_CONTENTS) {
    WebContents* web_contents =
        dispatcher()->delegate()->GetAssociatedWebContents();
    CHECK(web_contents);
    ExtensionWebUI* web_ui =
        static_cast<ExtensionWebUI*>(web_contents->GetWebUI()->GetController());
    CHECK(web_ui);
    BookmarkManagerPrivateDragEventRouter* router =
        web_ui->bookmark_manager_private_drag_event_router();

    DCHECK(router);
    const BookmarkNodeData* drag_data = router->GetBookmarkNodeData();
    if (drag_data == NULL) {
      NOTREACHED() <<"Somehow we're dropping null bookmark data";
      return false;
    }
    chrome::DropBookmarks(GetProfile(), *drag_data, drop_parent, drop_index);

    router->ClearBookmarkNodeData();
    return true;
  } else {
    NOTREACHED();
    return false;
  }
}

bool BookmarkManagerPrivateGetSubtreeFunction::RunImpl() {
  scoped_ptr<GetSubtree::Params> params(GetSubtree::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  const BookmarkNode* node = NULL;

  if (params->id == "") {
    BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());
    node = model->root_node();
  } else {
    node = GetBookmarkNodeFromId(params->id);
    if (!node)
      return false;
  }

  std::vector<linked_ptr<api::bookmarks::BookmarkTreeNode> > nodes;
  if (params->folders_only)
    bookmark_api_helpers::AddNodeFoldersOnly(node, &nodes, true);
  else
    bookmark_api_helpers::AddNode(node, &nodes, true);
  results_ = GetSubtree::Results::Create(nodes);
  return true;
}

bool BookmarkManagerPrivateCanEditFunction::RunImpl() {
  PrefService* prefs = user_prefs::UserPrefs::Get(GetProfile());
  SetResult(new base::FundamentalValue(
      prefs->GetBoolean(prefs::kEditBookmarksEnabled)));
  return true;
}

bool BookmarkManagerPrivateRecordLaunchFunction::RunImpl() {
  RecordBookmarkLaunch(NULL, BOOKMARK_LAUNCH_LOCATION_MANAGER);
  return true;
}

bool BookmarkManagerPrivateGetMetaInfoFunction::RunImpl() {
  scoped_ptr<GetMetaInfo::Params> params(GetMetaInfo::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  const BookmarkNode* node = GetBookmarkNodeFromId(params->id);
  if (!node)
    return false;

  std::string value;
  if (node->GetMetaInfo(params->key, &value))
    results_ = GetMetaInfo::Results::Create(value);
  return true;
}

bool BookmarkManagerPrivateSetMetaInfoFunction::RunImpl() {
  scoped_ptr<SetMetaInfo::Params> params(SetMetaInfo::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  const BookmarkNode* node = GetBookmarkNodeFromId(params->id);
  if (!node)
    return false;

  BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());
  model->SetNodeMetaInfo(node, params->key, params->value);
  return true;
}

bool BookmarkManagerPrivateCanOpenNewWindowsFunction::RunImpl() {
  bool can_open_new_windows = true;

#if defined(OS_WIN)
  if (win8::IsSingleWindowMetroMode())
    can_open_new_windows = false;
#endif  // OS_WIN

  SetResult(new base::FundamentalValue(can_open_new_windows));
  return true;
}

bool BookmarkManagerPrivateRemoveTreesFunction::RunImpl() {
  scoped_ptr<RemoveTrees::Params> params(RemoveTrees::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

#if !defined(OS_ANDROID)
  ScopedGroupBookmarkActions group_deletes(GetProfile());
#endif
  BookmarkModel* model = BookmarkModelFactory::GetForProfile(GetProfile());
  int64 id;
  for (size_t i = 0; i < params->id_list.size(); ++i) {
    if (!GetBookmarkIdAsInt64(params->id_list[i], &id))
      return false;
    if (!bookmark_api_helpers::RemoveNode(model, id, true, &error_))
      return false;
  }

  return true;
}

bool BookmarkManagerPrivateUndoFunction::RunImpl() {
#if !defined(OS_ANDROID)
  BookmarkUndoServiceFactory::GetForProfile(GetProfile())->undo_manager()->
      Undo();
#endif

  return true;
}

bool BookmarkManagerPrivateRedoFunction::RunImpl() {
#if !defined(OS_ANDROID)
  BookmarkUndoServiceFactory::GetForProfile(GetProfile())->undo_manager()->
      Redo();
#endif

  return true;
}

bool BookmarkManagerPrivateGetUndoInfoFunction::RunImpl() {
#if !defined(OS_ANDROID)
  UndoManager* undo_manager =
      BookmarkUndoServiceFactory::GetForProfile(GetProfile())->undo_manager();

  UndoInfo::Results::Result result;
  result.enabled = undo_manager->undo_count() > 0;
  result.label = base::UTF16ToUTF8(undo_manager->GetUndoLabel());

  results_ = UndoInfo::Results::Create(result);
#endif  // !defined(OS_ANDROID)

  return true;
}

bool BookmarkManagerPrivateGetRedoInfoFunction::RunImpl() {
#if !defined(OS_ANDROID)
  UndoManager* undo_manager =
      BookmarkUndoServiceFactory::GetForProfile(GetProfile())->undo_manager();

  RedoInfo::Results::Result result;
  result.enabled = undo_manager->redo_count() > 0;
  result.label = base::UTF16ToUTF8(undo_manager->GetRedoLabel());

  results_ = RedoInfo::Results::Create(result);
#endif  // !defined(OS_ANDROID)

  return true;
}

}  // namespace extensions
