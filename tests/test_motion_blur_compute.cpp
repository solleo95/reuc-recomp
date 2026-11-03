// Headless Deck oracle/benchmark: original SPIR-V vs candidate; no game or owner settings.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(VkResult r) { if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan result "+std::to_string(r)); }
struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; uint32_t w,h; };
struct Params { uint32_t output[2],render[2]; float shutter; uint32_t samples,tile,full; };
struct Gpu {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{}; VkCommandPool pool{}; VkCommandBuffer command{};
    VkDescriptorSetLayout setLayout{}; VkPipelineLayout layout{}; VkDescriptorPool descriptors{}; VkDescriptorSet set{};
    VkBuffer buffer{}; VkDeviceMemory host{}; unsigned char* mapped{}; VkQueryPool queries{};
    std::array<VkShaderModule,4> modules{}; std::array<VkPipeline,4> pipelines{}; float period{}; bool tileKernels=false;
    uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
        for(uint32_t i=0;i<memory.memoryTypeCount;++i) if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("memory type unavailable");
    }
    void initialize(char** files,char** tileFiles=nullptr) {
        tileKernels=tileFiles!=nullptr;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app; check(vkCreateInstance(&ci,nullptr,&instance));
        uint32_t count=0; check(vkEnumeratePhysicalDevices(instance,&count,nullptr));
        std::vector<VkPhysicalDevice> devices(count); check(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
        if(devices.empty()) throw std::runtime_error("no GPU"); physical=devices[0];
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical,&props); period=props.limits.timestampPeriod;
        std::printf("GPU: %s\n",props.deviceName); vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data()); uint32_t family=0;
        while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family;
        if(family==count || !families[family].timestampValidBits) throw std::runtime_error("compute timestamps unavailable");
        float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkPhysicalDeviceFeatures features{}; features.shaderStorageImageExtendedFormats=VK_TRUE;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi; di.pEnabledFeatures=&features;
        check(vkCreateDevice(physical,&di,nullptr,&device)); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex=family; pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; check(vkCreateCommandPool(device,&pi,nullptr,&pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1; check(vkAllocateCommandBuffers(device,&ai,&command));
        VkDescriptorSetLayoutBinding bindings[5]{};
        for(uint32_t i=0;i<5;++i) bindings[i]={i+1,(i==2||i==4)?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; si.bindingCount=5; si.pBindings=bindings; check(vkCreateDescriptorSetLayout(device,&si,nullptr,&setLayout));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.setLayoutCount=1; li.pSetLayouts=&setLayout; li.pushConstantRangeCount=1; li.pPushConstantRanges=&push; check(vkCreatePipelineLayout(device,&li,nullptr,&layout));
        for(int i=0;i<(tileKernels?4:2);++i) {
            std::ifstream f(i<2 ? files[i] : tileFiles[i-2],std::ios::binary|std::ios::ate); if(!f) throw std::runtime_error("missing shader");
            const auto size=f.tellg(); if(size<=0 || size%4) throw std::runtime_error("invalid SPIR-V size");
            std::vector<uint32_t> code(size_t(size)/4); f.seekg(0); f.read(reinterpret_cast<char*>(code.data()),size); if(!f) throw std::runtime_error("shader read failed");
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize=size_t(size); mi.pCode=code.data(); check(vkCreateShaderModule(device,&mi,nullptr,&modules[i]));
            VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules[i],i<2 ? "blurMain" : "tileMain",nullptr}; pc.layout=layout;
            check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,nullptr,&pipelines[i]));
        }
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=2; dp.pPoolSizes=sizes; check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors));
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=descriptors; da.descriptorSetCount=1; da.pSetLayouts=&setLayout; check(vkAllocateDescriptorSets(device,&da,&set));
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=32*1024*1024; bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT; check(vkCreateBuffer(device,&bi,nullptr,&buffer));
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buffer,&req);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize=req.size; ma.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); check(vkAllocateMemory(device,&ma,nullptr,&host)); check(vkBindBufferMemory(device,buffer,host,0)); check(vkMapMemory(device,host,0,VK_WHOLE_SIZE,0,reinterpret_cast<void**>(&mapped)));
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qp.queryType=VK_QUERY_TYPE_TIMESTAMP; qp.queryCount=2; check(vkCreateQueryPool(device,&qp,nullptr,&queries));
    }
    Image make(VkFormat format,uint32_t w,uint32_t h) {
        Image out{}; out.w=w; out.h=h;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=format; ci.extent={w,h,1}; ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vkCreateImage(device,&ci,nullptr,&out.image)); VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device,out.image,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); check(vkAllocateMemory(device,&ai,nullptr,&out.memory)); check(vkBindImageMemory(device,out.image,out.memory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=out.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; check(vkCreateImageView(device,&vi,nullptr,&out.view)); return out;
    }
    void destroy(Image& image) { vkDestroyImageView(device,image.view,nullptr); vkDestroyImage(device,image.image,nullptr); vkFreeMemory(device,image.memory,nullptr); }
    void begin() { check(vkResetCommandBuffer(command,0)); VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; check(vkBeginCommandBuffer(command,&bi)); }
    void finish() { check(vkEndCommandBuffer(command)); VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&command; check(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)); check(vkQueueWaitIdle(queue)); }
    void barrier() { VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr); }
    VkBufferImageCopy region(const Image& image,size_t offset) { VkBufferImageCopy r{}; r.bufferOffset=offset; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent={image.w,image.h,1}; return r; }
    std::string colour;
    void test(uint32_t w,uint32_t h,uint32_t rw,uint32_t rh,int pattern,uint32_t samples,uint32_t full,bool timing,const std::vector<std::array<float,2>>* field=nullptr) {
        std::array<Image,4> images={make(VK_FORMAT_R8G8B8A8_UNORM,w,h),make(VK_FORMAT_R32G32_SFLOAT,rw,rh),make(VK_FORMAT_R8G8B8A8_UNORM,w,h),make(tileKernels ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_R32G32_SFLOAT,(rw+15)/16,(rh+15)/16)};
        const size_t bytes=size_t(w)*h*4, motionOffset=(bytes+255)&~size_t(255), tileOffset=(motionOffset+size_t(rw)*rh*8+255)&~size_t(255), outputOffset=(tileOffset+size_t(images[3].w)*images[3].h*8+255)&~size_t(255);
        if(outputOffset+bytes>32*1024*1024) throw std::runtime_error("fixture buffer too small");
        for(size_t i=0;i<bytes;++i) mapped[i]=uint8_t((i*71+(i/4/w)*13)%256);
        if(field && !colour.empty()) {   // a real scene capture (raw RGBA, w*h*4 bytes) instead of the synthetic noise, which exaggerates sampling differences
            std::ifstream scene(colour,std::ios::binary); scene.read(reinterpret_cast<char*>(mapped),std::streamsize(bytes));
            if(size_t(scene.gcount())!=bytes) throw std::runtime_error("colour file is not "+std::to_string(bytes)+" bytes");
        }
        auto vector=[&](uint32_t x,uint32_t y) {
            if(field) return (*field)[size_t(y)*rw+x];
            if(pattern==0) return std::array<float,2>{0,0};
            if(pattern==5) {
                if((x+y)&1) return std::array<float,2>{0,0};
                constexpr std::array<std::array<float,2>,4> tied={{{12,0},{-12,0},{0,12},{0,-12}}};
                return tied[(x/16+y/16)%4];
            }
            const float scale=pattern==1?2.25f:pattern==2?13.5f:pattern==3?91.f:8192.f;
            return std::array<float,2>{((x/7+y/5)%3==0?0.f:scale)*((x/11)%2?-1.f:1.f),scale*float(int(y%7)-3)/7};
        };
        for(uint32_t y=0;y<rh;++y) for(uint32_t x=0;x<rw;++x) { auto v=vector(x,y); std::memcpy(mapped+motionOffset+(size_t(y)*rw+x)*8,v.data(),8); }
        for(uint32_t ty=0;ty<images[3].h;++ty) for(uint32_t tx=0;tx<images[3].w;++tx) {
            std::array<float,2> best{};
            for(uint32_t y=0;y<16;y+=2) for(uint32_t x=0;x<16;x+=2) if(tx*16+x<rw && ty*16+y<rh) { auto v=vector(tx*16+x,ty*16+y); if(v[0]*v[0]+v[1]*v[1]>best[0]*best[0]+best[1]*best[1]) best=v; }
            std::memcpy(mapped+tileOffset+(size_t(ty)*images[3].w+tx)*8,best.data(),8);
        }
        begin();
        for(auto& image:images) { if(!image.image) continue; VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_GENERAL; b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=image.image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b); }
        for(auto [index,offset]:std::array<std::pair<int,size_t>,3>{{{0,0},{1,motionOffset},{3,tileOffset}}}) { if(tileKernels && index==3) continue; auto r=region(images[index],offset); vkCmdCopyBufferToImage(command,buffer,images[index].image,VK_IMAGE_LAYOUT_GENERAL,1,&r); }
        barrier(); finish();
        const int mapping[]={0,1,2,3,3};
        for(uint32_t i=0;i<5;++i) { VkDescriptorImageInfo info{VK_NULL_HANDLE,images[mapping[i]].view,VK_IMAGE_LAYOUT_GENERAL}; VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set; write.dstBinding=i+1; write.descriptorCount=1; write.descriptorType=(i==2||i==4)?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; write.pImageInfo=&info; vkUpdateDescriptorSets(device,1,&write,0,nullptr); }
        Params params{{w,h},{rw,rh},samples==8?0.3f:samples==12?0.5f:0.7f,samples,16,full};
        std::vector<unsigned char> reference; double elapsed[2]{}; int repetitions=timing?8:1;
        for(int round=0;round<repetitions;++round) for(int order=0;order<2;++order) {
            const int variant=(round%2)?1-order:order;
            begin(); barrier();
            { VkImageCopy same{}; same.srcSubresource=same.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; same.extent={w,h,1};   // the game's output image is the picture the source copy was taken from (UC_BLUR_IN_PLACE)
              vkCmdCopyImage(command,images[0].image,VK_IMAGE_LAYOUT_GENERAL,images[2].image,VK_IMAGE_LAYOUT_GENERAL,1,&same); barrier(); }
            vkCmdResetQueryPool(command,queries,0,2); vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr); vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
            if(tileKernels) { vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[variant+2]); if(variant==1 && std::getenv("UC_TILE_GROUP")) vkCmdDispatch(command,images[3].w,images[3].h,1); else vkCmdDispatch(command,(images[3].w+7)/8,(images[3].h+7)/8,1); barrier(); }   // UC_TILE_GROUP: the candidate tile kernel (UC_BLUR_TILE_GROUP) runs a workgroup per tile
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[variant]);
            vkCmdDispatch(command,(w+7)/8,(h+7)/8,1); vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1); barrier();
            auto r=region(images[2],outputOffset); vkCmdCopyImageToBuffer(command,images[2].image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&r);
            VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; readback.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; readback.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,nullptr,0,nullptr); finish();
            uint64_t stamps[2]{}; check(vkGetQueryPoolResults(device,queries,0,2,sizeof stamps,stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)); elapsed[variant]+=double(stamps[1]-stamps[0])*period/1e6;
            if(reference.empty()) reference.assign(mapped+outputOffset,mapped+outputOffset+bytes);
            else if(std::memcmp(reference.data(),mapped+outputOffset,bytes)) {
                // UC_BLUR_TOLERANCE=N accepts a candidate that differs by at most N levels per channel (a deliberate sampling change); the default is byte-exact.
                size_t different=0; int largest=0; double total=0;
                for(size_t i=0;i<bytes;++i) { const int d=std::abs(int(reference[i])-int(mapped[outputOffset+i])); different+=d!=0; largest=std::max(largest,d); total+=d; }
                const char* limit=std::getenv("UC_BLUR_TOLERANCE");
                if(!limit || largest>std::atoi(limit)) throw std::runtime_error("pixel mismatch pattern "+std::to_string(pattern)+" bytes "+std::to_string(different)+" largest "+std::to_string(largest));
                std::printf("DIFF pattern%d samples%u full%u bytes %zu/%zu largest %d mean %.5f\n",pattern,samples,full,different,bytes,largest,total/double(bytes));
            }
        }
        std::printf("PASS %ux%u render%ux%u pattern%d samples%u full%u GPU %.4f -> %.4f ms\n",w,h,rw,rh,pattern,samples,full,elapsed[0]/repetitions,elapsed[1]/repetitions); std::fflush(stdout);
        for(auto& image:images) if(image.image) destroy(image);
    }
    ~Gpu() {
        if(device) { vkDeviceWaitIdle(device); if(mapped) vkUnmapMemory(device,host); vkDestroyQueryPool(device,queries,nullptr); vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,host,nullptr); vkDestroyDescriptorPool(device,descriptors,nullptr); for(auto p:pipelines) vkDestroyPipeline(device,p,nullptr); for(auto m:modules) vkDestroyShaderModule(device,m,nullptr); vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDescriptorSetLayout(device,setLayout,nullptr); vkDestroyCommandPool(device,pool,nullptr); vkDestroyDevice(device,nullptr); }
        if(instance) vkDestroyInstance(instance,nullptr);
    }
};
int main(int argc,char** argv) {
    if(argc<3) { std::fprintf(stderr,"usage: test_motion_blur_compute reference.spv candidate.spv [--tiles reference-tile.spv candidate-tile.spv] [capture.motion ...]\n"); return 2; }
    try {
        const bool tiles=argc>3 && std::string(argv[3])=="--tiles";
        if(tiles && argc<6) throw std::runtime_error("--tiles requires two shaders");
        const int captureAt=tiles?6:3;
        Gpu gpu; gpu.initialize(argv+1,tiles ? argv+4 : nullptr);
        for(int pattern=0;pattern<6;++pattern) for(uint32_t count:{8u,12u,20u,32u}) for(uint32_t full:{0u,1u}) {
            gpu.test(65,39,49,29,pattern,count,full,false);
            gpu.test(65,39,65,39,pattern,count,full,false);
            gpu.test(16383,1,16383,1,pattern,count,full,false);
            gpu.test(1,1,1,1,pattern,count,full,false);
        }
        for(int pattern=0;pattern<6;++pattern) gpu.test(1280,720,1280,720,pattern,32,0,true);
        for(int i=captureAt;i<argc;++i) {
            std::ifstream file(argv[i],std::ios::binary);
            uint32_t size[2]{}; file.read(reinterpret_cast<char*>(size),sizeof size);
            if(!file || !size[0] || !size[1] || uint64_t(size[0])*size[1]>2'000'000) throw std::runtime_error("invalid motion capture size");
            std::vector<std::array<float,2>> field(size_t(size[0])*size[1]);
            file.read(reinterpret_cast<char*>(field.data()),std::streamsize(field.size()*sizeof(field[0])));
            if(!file || file.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("invalid motion capture payload");
            for(auto v:field) if(!std::isfinite(v[0]) || !std::isfinite(v[1])) throw std::runtime_error("non-finite motion capture");
            std::printf("capture: %s\n",argv[i]);
            { std::string rgba=argv[i]; rgba=rgba.substr(0,rgba.rfind('.'))+".rgba"; if(std::ifstream(rgba,std::ios::binary).good()) gpu.colour=rgba; else gpu.colour.clear(); }
            // First train scene: viewport 640x360 inside the 640x480 EFB, upscaler off.
            size[1]=(size[1]*3+2)/4;
            field.resize(size_t(size[0])*size[1]);
            gpu.test(size[0],size[1],size[0],size[1],6,32,0,true,&field);
        }
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
