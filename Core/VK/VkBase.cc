/**
 * @brief Vulkan Application
 * This code is licensed under the MIT license (MIT)
 * (http://opensource.org/licenses/MIT)
 */

//*-----------------------------------------------------------------------------
// Including files
//*-----------------------------------------------------------------------------

#include "VkBase.h"

#include <boost/assert.hpp>
#include <map>
#include <spdlog/spdlog.h>

#include "VK/Common.h"
#include "VK/Initializer.h"
#include "VK/Utils.h"
#include "VK/Image/Image.h"
#include "VK/Image/ImageView.h"

//*-----------------------------------------------------------------------------
// Init & Deinit
//*-----------------------------------------------------------------------------

void VkBase::OnInit(const nlohmann::json &conf, GLFWwindow *hwnd) {
  config = conf;
  window = hwnd;

  const auto appName = config["AppName"].get<std::string>();

  CreateInstance(appName.c_str());
#if !defined(NDEBUG)
  debugMessenger.Setup(instance);
#endif
  VkPhysicalDevice physicalDevice = SelectPhysicalDevice();
  swapchain.Init(instance, window, physicalDevice);
  device.Init(physicalDevice);
  VK_CHECK_RESULT(device.CreateLogicalDevice(GetEnabledFeatures(),
                                             GetEnabledDeviceExtensions()));

  // デバイスからグラフィックスキューを取得します。
  vkGetDeviceQueue(device, device.queueFamilyIndices.graphics, 0, &queue);
  CreateSemaphores();

  OnPostInit();
}

void VkBase::OnPostInit() {
  const auto width = config["Width"].get<int>();
  const auto height = config["Height"].get<int>();

  CreateSwapchain(width, height);

  CreateCommandPool();
  CreateCommandBuffers();
  CreateFence();
  SetupDepthStencil();
  SetupRenderPass();
  CreatePipelineCache();
  SetupFramebuffers();
}

void VkBase::OnPreDestroy() {}

void VkBase::OnDestroy() {
  OnPreDestroy();

  swapchain.Destroy(instance, device);
  if (descriptorPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  }
  DestroyCommandBuffers();
  vkDestroyRenderPass(device, renderPass, nullptr);
  for (auto &framebuffer : framebuffers) {
    vkDestroyFramebuffer(device, framebuffer, nullptr);
  }

  DestroyDepthStencil();

  vkDestroyPipelineCache(device, pipelineCache, nullptr);
  vkDestroyCommandPool(device, cmdPool, nullptr);
  DestroySyncObjects();

  device.Destroy();
#if !defined(NDEBUG)
  debugMessenger.Cleanup(instance);
#endif
  vkDestroyInstance(instance, nullptr);
}

//*-----------------------------------------------------------------------------
// Update
//*-----------------------------------------------------------------------------

void VkBase::OnUpdate(float) {}

//*-----------------------------------------------------------------------------
// Render
//*-----------------------------------------------------------------------------

void VkBase::OnRender() { VkBase::RenderFrame(); }

void VkBase::RenderFrame() {
  VkBase::PrepareFrame();
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
  VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
  VkBase::SubmitFrame();
}

void VkBase::PrepareFrame() {
  // スワップチェーンの次の画像を取得します。(バック/フロントバッファ)
  VkResult result = swapchain.AcquiredNextImage(
      device, semaphores.presentComplete, &currentBuffer);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    ResizeWindow();
    return;
  } else {
    VK_CHECK_RESULT(result);
  }
}

void VkBase::SubmitFrame() {
  VkResult result =
      swapchain.QueuePresent(queue, currentBuffer, semaphores.renderComplete);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
      isFramebufferResized) {
    isFramebufferResized = false;
    ResizeWindow();
    return;
  } else {
    VK_CHECK_RESULT(result);
  }
  VK_CHECK_RESULT(vkQueueWaitIdle(queue));
}

void VkBase::WaitIdle() const { vkDeviceWaitIdle(device); }

//*-----------------------------------------------------------------------------
// Resize window
//*-----------------------------------------------------------------------------

void VkBase::OnResized(GLFWwindow *window, int, int) {
  auto app = reinterpret_cast<VkBase *>(glfwGetWindowUserPointer(window));
  app->isFramebufferResized = true;
}

void VkBase::ResizeWindow() {
  // Windowが最小化されている場合framebufferのresizeが行われるまで待ちます。
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);
  while (width == 0 || height == 0) {
    glfwGetFramebufferSize(window, &width, &height);
    glfwWaitEvents();
  }

  // リソースを破棄する前にDeviceがすべての作業を終わらせている必要があります。
  vkDeviceWaitIdle(device);

  // Swap chain の再生成を行います。
  swapchain.Create(device, width, height);

  // Frame buffers の再生成を行います。
  DestroyDepthStencil();
  SetupDepthStencil();
  for (auto &framebuffer : framebuffers) {
    vkDestroyFramebuffer(device, framebuffer, nullptr);
  }
  SetupFramebuffers();

  // Frame buffersの再生成後にCommand buffersも再生成する必要があります。
  DestroyCommandBuffers();
  CreateCommandBuffers();
  BuildCommandBuffers();

  vkDeviceWaitIdle(device);

  ViewChanged();
}

//*-----------------------------------------------------------------------------
// Vulkan Instance
//*-----------------------------------------------------------------------------

void VkBase::CreateInstance(const char *appName) {
  VkApplicationInfo info{};
  info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  info.pApplicationName = appName;
  info.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
  info.pEngineName = "";
  info.engineVersion = VK_MAKE_VERSION(0, 0, 1);
  info.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &info;

  uint32_t glfwExtensionCount = 0;
  const char **glfwExtensions =
      glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
  std::vector<const char *> extensions(glfwExtensions,
                                       glfwExtensions + glfwExtensionCount);
  if (isEnableValidationLayers_) {
    extensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    spdlog::info("Required extensions:");
    for (const auto extension : extensions) {
      spdlog::info(" - {}", extension);
    }
  }
  create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  create.ppEnabledExtensionNames = extensions.data();

  if (isEnableValidationLayers_) {
    spdlog::info("Enabling validation");
    create.enabledLayerCount = static_cast<uint32_t>(validationLayers_.size());
    create.ppEnabledLayerNames = validationLayers_.data();
  } else {
    create.enabledLayerCount = 0;
  }

  VK_CHECK_RESULT(vkCreateInstance(&create, nullptr, &instance));
}

VkPhysicalDevice VkBase::SelectPhysicalDevice() const {
  uint32_t size = 0;
  vkEnumeratePhysicalDevices(instance, &size, nullptr);
  BOOST_ASSERT_MSG(size != 0, "Failed to find any physical device!");

  std::vector<VkPhysicalDevice> devices(size);
  vkEnumeratePhysicalDevices(instance, &size, devices.data());
#if !defined(NDEBUG)
  spdlog::info("Found {} physical devices", size);
#endif
  std::multimap<float, VkPhysicalDevice> scores;
  for (const auto &dev : devices) {
    scores.emplace(CalcDeviceScore(dev, GetEnabledDeviceExtensions()),
                   dev);
  }
  BOOST_ASSERT_MSG(scores.rbegin()->first >= 0.0000001f,
                   "Failed to find suitable physical device");

  return scores.rbegin()->second;
}

//*-----------------------------------------------------------------------------
// Vulkan Fixed functions
//*-----------------------------------------------------------------------------

void VkBase::CreateSwapchain(int w, int h) { swapchain.Create(device, w, h); }

void VkBase::CreatePipelineCache() {
  VkPipelineCacheCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  VK_CHECK_RESULT(
      vkCreatePipelineCache(device, &create, nullptr, &pipelineCache));
}

void VkBase::CreateCommandPool() {
  VkCommandPoolCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  create.queueFamilyIndex = swapchain.queueFamilyIndex;
  create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VK_CHECK_RESULT(vkCreateCommandPool(device, &create, nullptr, &cmdPool));
}

void VkBase::CreateCommandBuffers() {
  drawCmdBuffers.resize(swapchain.images.size());

  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = cmdPool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = static_cast<uint32_t>(drawCmdBuffers.size());
  VK_CHECK_RESULT(
      vkAllocateCommandBuffers(device, &alloc, drawCmdBuffers.data()));
}

void VkBase::DestroyCommandBuffers() {
  vkFreeCommandBuffers(device, cmdPool,
                       static_cast<uint32_t>(drawCmdBuffers.size()),
                       drawCmdBuffers.data());
}

void VkBase::CreateSemaphores() {
  VkSemaphoreCreateInfo semaphoreCreateInfo =
      Initializer::SemaphoreCreateInfo();
  VK_CHECK_RESULT(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr,
                                    &semaphores.presentComplete));
  VK_CHECK_RESULT(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr,
                                    &semaphores.renderComplete));

  submitInfo = Initializer::SubmitInfo();
  submitInfo.pWaitDstStageMask = &submitPipelineStages;
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = &semaphores.presentComplete;
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = &semaphores.renderComplete;
}

void VkBase::CreateFence() {
  VkFenceCreateInfo create =
      Initializer::FenceCreateInfo(VK_FENCE_CREATE_SIGNALED_BIT);
  waitFences.resize(drawCmdBuffers.size());
  for (auto &fence : waitFences) {
    VK_CHECK_RESULT(vkCreateFence(device, &create, nullptr, &fence));
  }
}

void VkBase::DestroySyncObjects() {
  for (auto &fence : waitFences) {
    vkDestroyFence(device, fence, nullptr);
  }
  vkDestroySemaphore(device, semaphores.renderComplete, nullptr);
  vkDestroySemaphore(device, semaphores.presentComplete, nullptr);
}

void VkBase::DestroyDepthStencil() {
  vkDestroyImageView(device, depthStencil.view, nullptr);
  vkDestroyImage(device, depthStencil.image, nullptr);
  vkFreeMemory(device, depthStencil.memory, nullptr);
}

//*-----------------------------------------------------------------------------
// Vulkan virtual functions
//*-----------------------------------------------------------------------------

/**
 * @brief フレームバッファで使用される深度(ステンシル)バッファを生成します。
 */
void VkBase::SetupDepthStencil() {
  const auto depthFormat = device.FindSupportedDepthFormat();
  Image::Create(device, swapchain.extent.width, swapchain.extent.height, 0,
                depthFormat, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthStencil.image, depthStencil.memory);
  depthStencil.view = ImageView::Create(device, depthStencil.image, VK_IMAGE_VIEW_TYPE_2D,
                           depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
}

/**
 * @brief スワップチェーンのイメージごとにフレームバッファを生成します。
 */
void VkBase::SetupFramebuffers() {
  std::array<VkImageView, 2> attachments{VK_NULL_HANDLE, depthStencil.view};

  // Depth/Stencil attachmentをすべてのframebufferに適用します。
  VkFramebufferCreateInfo create = Initializer::FramebufferCreateInfo();

  // すべてのフレームバッファは同じのレンダーパス設定を使用します。
  create.renderPass = renderPass;
  create.attachmentCount = static_cast<uint32_t>(attachments.size());
  create.pAttachments = attachments.data();
  create.width = swapchain.extent.width;
  create.height = swapchain.extent.height;
  create.layers = 1;

  // スワップチェーン内のすべてのイメージのフレームバッファを生成します。
  framebuffers.resize(swapchain.views.size());
  for (size_t i = 0; i < framebuffers.size(); i++) {
    attachments[0] = swapchain.views[i];
    VK_CHECK_RESULT(
        vkCreateFramebuffer(device, &create, nullptr, &framebuffers[i]));
  }
}

/**
 * @brief
 * レンダリングパスの設定(ここでは1つのサブパスを持つ単一のレンダーパスを使用します。)
 * @note
 * レンダリングパスはVulkanの新しい概念です。それらはレンダリング中に使用されるアタッチメントを記述し、アタッチメントの依存関係を持つ複数のサブパスを含む場合があります。<br>
 * これにより、ドライバーはレンダリングがどのように見えるかを事前に知ることができ、特にタイルベースのレンダラー(複数のサブパスを使用)は最適化する良い機会です。<br>
 * サブパスの依存関係を使用すると使用するアタッチメントの暗黙的なレイアウト遷移も追加されるため、明示的な画像のメモリバリアを追加する必要はありません。
 */
void VkBase::SetupRenderPass() {

  // カラーアタッチメント
  VkAttachmentDescription color{};
  color.format = swapchain.format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  // デプスアタッチメント
  VkAttachmentDescription depth{};
  depth.format = device.FindSupportedDepthFormat();
  depth.samples = VK_SAMPLE_COUNT_1_BIT;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  // アタッチメントの参照を設定します。
  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthRef{};
  depthRef.attachment = 1;
  depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  // Setup a single subpass reference
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;
  subpass.pDepthStencilAttachment = &depthRef;
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = nullptr;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = nullptr;
  subpass.pResolveAttachments = nullptr;

  // サブパスの依存関係を設定します。
  // これらはアタッチメントの記述子で指定された暗黙のアタッチメントレイアウト遷移を追加します。
  // 実際の使用するレイアウトは、アタッチメントリファレンスで指定されたレイアウトを通じて保持されます。
  // 各サブパスの依存関係はsrcStageMask、dstStageMask、srcAccessMask、dstAccessMask(およびdependencyFlagsが設定されている)
  // によって記述されている入力サブパスと出力サブパスの間にメモリと実行の依存関係を導入します。
  // NOTE:VK_SUBPASS_EXTERNALは、実際のレンダーパスの外部で実行されるすべてのコマンドを参照する特別な定数です。
  std::array<VkSubpassDependency, 2> dependencies{};

  // レンダーパスの開始時の依存関係(First dependency)
  // 最終レイアウトから開始レイアウトへの移行を行います。
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  // レンダーパスの終了時の依存関係(Second dependency)
  // 開始レイアウトから終了レイアウトへの移行を行います。
  // これは、暗黙的なサブパスの依存関係と同じですが、ここで明示的に行います。
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  std::vector<VkAttachmentDescription> attachments{color, depth};

  // 実際のレンダーパスを作成します。
  VkRenderPassCreateInfo create = Initializer::RenderPassCreateInfo();
  create.attachmentCount = static_cast<uint32_t>(attachments.size());
  create.pAttachments = attachments.data();
  create.subpassCount = 1;
  create.pSubpasses = &subpass;
  create.dependencyCount = static_cast<uint32_t>(dependencies.size());
  create.pDependencies = dependencies.data();

  VK_CHECK_RESULT(vkCreateRenderPass(device, &create, nullptr, &renderPass));
}

void VkBase::BuildCommandBuffers() {}

void VkBase::ViewChanged() {}

VkPhysicalDeviceFeatures VkBase::GetEnabledFeatures() const {
  VkPhysicalDeviceFeatures enabledFeatures{};
  return enabledFeatures;
}

std::vector<const char *> VkBase::GetEnabledDeviceExtensions() const {
  return {};
}
