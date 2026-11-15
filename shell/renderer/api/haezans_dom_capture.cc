// Copyright (c) 2026 Haezan.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/renderer/api/haezans_dom_capture.h"

#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "shell/common/api/api.mojom.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/imagebitmap/image_bitmap.h"  // nogncheck
#include "third_party/blink/renderer/platform/bindings/script_state.h"  // nogncheck
#include "third_party/blink/renderer/platform/graphics/accelerated_static_bitmap_image.h"  // nogncheck
#include "third_party/skia/include/core/SkAlphaType.h"
#include "ui/gfx/geometry/rect.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace electron {
namespace {

class HaezansHost;
std::map<content::RenderFrame*, HaezansHost*>& HaezansHosts();

class HaezansHost : public content::RenderFrameObserver,
                    public mojom::HaezansDomCaptureClient {
 public:
  explicit HaezansHost(content::RenderFrame* render_frame)
      : content::RenderFrameObserver(render_frame) {}

  void Attach(v8::Isolate* isolate, v8::Local<v8::Context> context) {
    v8::Local<v8::Object> api = v8::Object::New(isolate);
    v8::Local<v8::Function> below =
        v8::Function::New(context, &HaezansHost::Below,
                          v8::External::New(isolate, this,
                                            v8::kExternalPointerTypeTagDefault))
            .ToLocalChecked();
    v8::Local<v8::Function> stop =
        v8::Function::New(context, &HaezansHost::Stop,
                          v8::External::New(isolate, this,
                                            v8::kExternalPointerTypeTagDefault))
            .ToLocalChecked();
    api->Set(context, v8::String::NewFromUtf8Literal(isolate, "below"), below)
        .Check();
    api->Set(context, v8::String::NewFromUtf8Literal(isolate, "stop"), stop)
        .Check();
    context->Global()
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate, "haezansDomCapture"),
              api)
        .Check();
  }

  void OnRegion(uint32_t token,
                const gfx::Rect& rect,
                gpu::ExportedSharedImage image,
                const gpu::SyncToken& sync_token) override {
    if (callback_.IsEmpty() || context_.IsEmpty()) {
      ReleaseCapture(token, gpu::SyncToken());
      return;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (!isolate) {
      ReleaseCapture(token, gpu::SyncToken());
      return;
    }
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = context_.Get(isolate);
    v8::Context::Scope context_scope(context);
    v8::TryCatch try_catch(isolate);

    bool released = false;
    scoped_refptr<blink::AcceleratedStaticBitmapImage> accelerated =
        blink::AcceleratedStaticBitmapImage::CreateFromExternalSharedImage(
            std::move(image), sync_token, kPremul_SkAlphaType,
            base::BindOnce(
                [](HaezansHost* host, uint32_t token, bool* released,
                   const gpu::SyncToken& release_token) {
                  *released = true;
                  host->ReleaseCapture(token, release_token);
                },
                this, token, &released));
    if (!released)
      ReleaseCapture(token, gpu::SyncToken());
    if (!accelerated)
      return;

    blink::ScriptState* script_state =
        blink::ScriptState::MaybeFrom(isolate, context);
    if (!script_state)
      return;
    auto* bitmap = blink::MakeGarbageCollected<blink::ImageBitmap>(
        scoped_refptr<blink::StaticBitmapImage>(std::move(accelerated)));
    v8::Local<v8::Value> wrapped = bitmap->ToV8(script_state);
    if (wrapped.IsEmpty())
      return;
    v8::Local<v8::Value> args[] = {
        v8::Integer::New(isolate, rect.x()),
        v8::Integer::New(isolate, rect.y()),
        v8::Integer::New(isolate, rect.width()),
        v8::Integer::New(isolate, rect.height()),
        wrapped,
    };
    std::ignore =
        callback_.Get(isolate)->Call(context, context->Global(), 5, args);
  }

  void ReleaseCapture(uint32_t token, const gpu::SyncToken& sync_token) {
    if (ipc_.is_bound())
      ipc_->ReleaseHaezansCapture(token, sync_token);
  }

 private:
  static void Below(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* isolate = info.GetIsolate();
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    auto* host = static_cast<HaezansHost*>(
        info.Data().As<v8::External>()->Value(
            v8::kExternalPointerTypeTagDefault));
    if (info.Length() < 2 || !info[1]->IsFunction())
      return;
    host->callback_.Reset(isolate, info[1].As<v8::Function>());
    host->context_.Reset(isolate, context);
    int32_t max_z = info[0]->Int32Value(context).FromMaybe(0);
    if (!host->ipc_.is_bound()) {
      host->render_frame()->GetRemoteAssociatedInterfaces()->GetInterface(
          &host->ipc_);
    }
    host->receiver_.reset();
    mojo::PendingRemote<mojom::HaezansDomCaptureClient> remote;
    host->receiver_.Bind(remote.InitWithNewPipeAndPassReceiver());
    host->ipc_->SetHaezansDomCapture(true, max_z, std::move(remote));
  }

  static void Stop(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* host = static_cast<HaezansHost*>(
        info.Data().As<v8::External>()->Value(
            v8::kExternalPointerTypeTagDefault));
    host->callback_.Reset();
    host->context_.Reset();
    if (!host->ipc_.is_bound())
      return;
    host->receiver_.reset();
    mojo::PendingRemote<mojom::HaezansDomCaptureClient> remote;
    host->receiver_.Bind(remote.InitWithNewPipeAndPassReceiver());
    host->ipc_->SetHaezansDomCapture(false, 0, std::move(remote));
  }

  void WillReleaseScriptContext(v8::Isolate* isolate,
                                v8::Local<v8::Context> context,
                                int32_t world_id) override {
    if (!context_.IsEmpty() && context == context_.Get(isolate)) {
      callback_.Reset();
      context_.Reset();
    }
  }

  void OnDestruct() override {
    HaezansHosts().erase(render_frame());
    delete this;
  }

  mojo::AssociatedRemote<mojom::ElectronApiIPC> ipc_;
  mojo::Receiver<mojom::HaezansDomCaptureClient> receiver_{this};
  v8::Global<v8::Function> callback_;
  v8::Global<v8::Context> context_;
};

std::map<content::RenderFrame*, HaezansHost*>& HaezansHosts() {
  static base::NoDestructor<std::map<content::RenderFrame*, HaezansHost*>>
      hosts;
  return *hosts;
}

}  // namespace

void InstallHaezansDomCapture(content::RenderFrame* render_frame,
                              v8::Isolate* isolate,
                              v8::Local<v8::Context> context) {
  if (!render_frame || !render_frame->IsMainFrame())
    return;
  blink::WebLocalFrame* web_frame = render_frame->GetWebFrame();
  if (!web_frame || web_frame->MainWorldScriptContext() != context)
    return;
  auto& hosts = HaezansHosts();
  HaezansHost* host = nullptr;
  auto it = hosts.find(render_frame);
  if (it == hosts.end()) {
    host = new HaezansHost(render_frame);
    hosts[render_frame] = host;
  } else {
    host = it->second;
  }
  host->Attach(isolate, context);
}

}  // namespace electron
