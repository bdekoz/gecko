/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/JSWindowActorBinding.h"
#include "mozilla/dom/JSWindowActorParent.h"
#include "mozilla/dom/WindowGlobalParent.h"
#include "mozilla/dom/MessageManagerBinding.h"

namespace mozilla {
namespace dom {

JSWindowActorParent::~JSWindowActorParent() { MOZ_ASSERT(!mManager); }

JSObject* JSWindowActorParent::WrapObject(JSContext* aCx,
                                          JS::Handle<JSObject*> aGivenProto) {
  return JSWindowActorParent_Binding::Wrap(aCx, this, aGivenProto);
}

WindowGlobalParent* JSWindowActorParent::GetManager() const { return mManager; }

void JSWindowActorParent::Init(const nsAString& aName,
                               WindowGlobalParent* aManager) {
  MOZ_ASSERT(!mManager, "Cannot Init() a JSWindowActorParent twice!");
  SetName(aName);
  mManager = aManager;
}

namespace {

class AsyncMessageToChild : public Runnable {
 public:
  AsyncMessageToChild(const JSWindowActorMessageMeta& aMetadata,
                      ipc::StructuredCloneData&& aData,
                      WindowGlobalParent* aManager)
      : mozilla::Runnable("WindowGlobalChild::HandleAsyncMessage"),
        mMetadata(aMetadata),
        mData(std::move(aData)),
        mManager(aManager) {}

  NS_IMETHOD Run() override {
    MOZ_ASSERT(NS_IsMainThread(), "Should be called on the main thread.");
    RefPtr<WindowGlobalChild> child = mManager->GetChildActor();
    if (child) {
      child->ReceiveRawMessage(mMetadata, std::move(mData));
    }
    return NS_OK;
  }

 private:
  JSWindowActorMessageMeta mMetadata;
  ipc::StructuredCloneData mData;
  RefPtr<WindowGlobalParent> mManager;
};

}  // anonymous namespace

void JSWindowActorParent::SendRawMessage(const JSWindowActorMessageMeta& aMeta,
                                         ipc::StructuredCloneData&& aData,
                                         ErrorResult& aRv) {
  if (NS_WARN_IF(!mCanSend || !mManager || mManager->IsClosed())) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  if (mManager->IsInProcess()) {
    nsCOMPtr<nsIRunnable> runnable =
        new AsyncMessageToChild(aMeta, std::move(aData), mManager);
    NS_DispatchToMainThread(runnable.forget());
    return;
  }

  // Cross-process case - send data over WindowGlobalParent to other side.
  ClonedMessageData msgData;
  RefPtr<BrowserParent> browserParent = mManager->GetBrowserParent();
  ContentParent* cp = browserParent->Manager();
  if (NS_WARN_IF(!aData.BuildClonedMessageDataForParent(cp, msgData))) {
    aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
    return;
  }

  if (NS_WARN_IF(!mManager->SendRawMessage(aMeta, msgData))) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return;
  }
}

CanonicalBrowsingContext* JSWindowActorParent::GetBrowsingContext(
    ErrorResult& aRv) {
  if (!mManager) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  return mManager->BrowsingContext();
}

void JSWindowActorParent::StartDestroy() {
  JSWindowActor::StartDestroy();
  mCanSend = false;
}

void JSWindowActorParent::AfterDestroy() {
  JSWindowActor::AfterDestroy();
  mManager = nullptr;
}

NS_IMPL_CYCLE_COLLECTION_INHERITED(JSWindowActorParent, JSWindowActor, mManager)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(JSWindowActorParent,
                                               JSWindowActor)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(JSWindowActorParent)
NS_INTERFACE_MAP_END_INHERITING(JSWindowActor)

NS_IMPL_ADDREF_INHERITED(JSWindowActorParent, JSWindowActor)
NS_IMPL_RELEASE_INHERITED(JSWindowActorParent, JSWindowActor)

}  // namespace dom
}  // namespace mozilla
