/*! Render Bundles

A render bundle is a prerecorded sequence of commands that can be replayed on a
command encoder with a single call. A single bundle can replayed any number of
times, on different encoders. Constructing a render bundle lets `wgpu` validate
and analyze its commands up front, so that replaying a bundle can be more
efficient than simply re-recording its commands each time.

One important property of render bundles is that the draw calls in a render
bundle depend solely on the pipeline and state established within the render
bundle itself. A draw call in a bundle will never use a vertex buffer, say, that
was set in the `RenderPass` before executing the bundle. We call this property
'isolation', in that a render bundle is somewhat isolated from the passes that
use it.

Render passes are also isolated from the effects of bundles. After executing a
render bundle, a render pass's pipeline, bind groups, and vertex and index
buffers are are unset, so the bundle cannot affect later draw calls in the pass.

Not all commands are available in bundles; for example, a render bundle may not
contain a [`RenderCommand::SetViewport`] command.

Most of `wgpu`'s backend graphics APIs have something like bundles. For example,
Vulkan calls them "secondary command buffers", and Metal calls them "indirect
command buffers". However, `wgpu`'s implementation of render bundles does not
take advantage of those underlying platform features. At the hal level, `wgpu`
render bundles just replay the commands.

## Render Bundle Lifecycle

To create a render bundle:

1) Create a [`RenderBundleEncoder`] by calling
   [`Global::device_create_render_bundle_encoder`][Gdcrbe].

2) Record commands in the `RenderBundleEncoder` using functions from the
   [`bundle_ffi`] module.

3) Call [`Global::render_bundle_encoder_finish`][Grbef], which analyzes and cleans up
   the command stream and returns a `RenderBundleId`.

4) Then, any number of times, call [`wgpu_render_pass_execute_bundles`][wrpeb] to
   execute the bundle as part of some render pass.

## Implementation

The most complex part of render bundles is the "finish" step, mostly implemented
in [`RenderBundleEncoder::finish`]. This consumes the commands stored in the
encoder's [`BasePass`], while validating everything, tracking the state,
dropping redundant or unnecessary commands, and presenting the results as a new
[`RenderBundle`]. It doesn't actually execute any commands.

This step also enforces the 'isolation' property mentioned above: every draw
call is checked to ensure that the resources it uses on were established since
the last time the pipeline was set. This means the bundle can be executed
verbatim without any state tracking.

### Execution

When the bundle is used in an actual render pass, `RenderBundle::execute` is
called. It goes through the commands and issues them into the native command
buffer. Thanks to isolation, it doesn't track any bind group invalidations or
index format changes.

[Gdcrbe]: crate::hub::Global::device_create_render_bundle_encoder
[Grbef]: crate::hub::Global::render_bundle_encoder_finish
[wrpeb]: crate::command::render_ffi::wgpu_render_pass_execute_bundles
!*/

#![allow(clippy::reversed_empty_ranges)]

use crate::{
    binding_model::{self, buffer_binding_type_alignment},
    command::{
        BasePass, BindGroupStateChange, DrawError, MapPassErr, PassErrorScope, RenderCommand,
        RenderCommandError, StateChange,
    },
    conv,
    device::{
        AttachmentData, Device, DeviceError, MissingDownlevelFlags, RenderPassContext,
        SHADER_STAGE_COUNT,
    },
    error::{ErrorFormatter, PrettyError},
    hub::{GlobalIdentityHandlerFactory, HalApi, Hub, Resource, Storage, Token},
    id,
    init_tracker::{BufferInitTrackerAction, MemoryInitKind, TextureInitTrackerAction},
    pipeline::{self, PipelineFlags},
    resource,
    track::RenderBundleScope,
    validation::check_buffer_usage,
    Label, LabelHelpers, LifeGuard, Stored,
};
use arrayvec::ArrayVec;
use std::{borrow::Cow, mem, num::NonZeroU32, ops::Range};
use thiserror::Error;

use hal::CommandEncoder as _;

/// Describes a [`RenderBundleEncoder`].
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash)]
#[cfg_attr(feature = "trace", derive(serde::Serialize))]
#[cfg_attr(feature = "replay", derive(serde::Deserialize))]
pub struct RenderBundleEncoderDescriptor<'a> {
    /// Debug label of the render bundle encoder. This will show up in graphics debuggers for easy identification.
    pub label: Label<'a>,
    /// The formats of the color attachments that this render bundle is capable to rendering to. This
    /// must match the formats of the color attachments in the renderpass this render bundle is executed in.
    pub color_formats: Cow<'a, [wgt::TextureFormat]>,
    /// Information about the depth attachment that this render bundle is capable to rendering to. The format
    /// must match the format of the depth attachments in the renderpass this render bundle is executed in.
    pub depth_stencil: Option<wgt::RenderBundleDepthStencil>,
    /// Sample count this render bundle is capable of rendering to. This must match the pipelines and
    /// the renderpasses it is used in.
    pub sample_count: u32,
    /// If this render bundle will rendering to multiple array layers in the attachments at the same time.
    pub multiview: Option<NonZeroU32>,
}

#[derive(Debug)]
#[cfg_attr(feature = "serial-pass", derive(serde::Deserialize, serde::Serialize))]
pub struct RenderBundleEncoder {
    base: BasePass<RenderCommand>,
    parent_id: id::DeviceId,
    pub(crate) context: RenderPassContext,
    pub(crate) is_ds_read_only: bool,

    // Resource binding dedupe state.
    #[cfg_attr(feature = "serial-pass", serde(skip))]
    current_bind_groups: BindGroupStateChange,
    #[cfg_attr(feature = "serial-pass", serde(skip))]
    current_pipeline: StateChange<id::RenderPipelineId>,
}

impl RenderBundleEncoder {
    pub fn new(
        desc: &RenderBundleEncoderDescriptor,
        parent_id: id::DeviceId,
        base: Option<BasePass<RenderCommand>>,
    ) -> Result<Self, CreateRenderBundleError> {
        //TODO: validate that attachment formats are renderable,
        // have expected aspects, support multisampling.
        Ok(Self {
            base: base.unwrap_or_else(|| BasePass::new(&desc.label)),
            parent_id,
            context: RenderPassContext {
                attachments: AttachmentData {
                    colors: if desc.color_formats.len() > hal::MAX_COLOR_TARGETS {
                        return Err(CreateRenderBundleError::TooManyColorAttachments);
                    } else {
                        desc.color_formats.iter().cloned().collect()
                    },
                    resolves: ArrayVec::new(),
                    depth_stencil: desc.depth_stencil.map(|ds| ds.format),
                },
                sample_count: {
                    let sc = desc.sample_count;
                    if sc == 0 || sc > 32 || !conv::is_power_of_two_u32(sc) {
                        return Err(CreateRenderBundleError::InvalidSampleCount(sc));
                    }
                    sc
                },
                multiview: desc.multiview,
            },
            is_ds_read_only: match desc.depth_stencil {
                Some(ds) => {
                    let aspects = hal::FormatAspects::from(ds.format);
                    (!aspects.contains(hal::FormatAspects::DEPTH) || ds.depth_read_only)
                        && (!aspects.contains(hal::FormatAspects::STENCIL) || ds.stencil_read_only)
                }
                None => false,
            },

            current_bind_groups: BindGroupStateChange::new(),
            current_pipeline: StateChange::new(),
        })
    }

    pub fn dummy(parent_id: id::DeviceId) -> Self {
        Self {
            base: BasePass::new(&None),
            parent_id,
            context: RenderPassContext {
                attachments: AttachmentData {
                    colors: ArrayVec::new(),
                    resolves: ArrayVec::new(),
                    depth_stencil: None,
                },
                sample_count: 0,
                multiview: None,
            },
            is_ds_read_only: false,

            current_bind_groups: BindGroupStateChange::new(),
            current_pipeline: StateChange::new(),
        }
    }

    #[cfg(feature = "trace")]
    pub(crate) fn to_base_pass(&self) -> BasePass<RenderCommand> {
        BasePass::from_ref(self.base.as_ref())
    }

    pub fn parent(&self) -> id::DeviceId {
        self.parent_id
    }

    /// Convert this encoder's commands into a [`RenderBundle`].
    ///
    /// We want executing a [`RenderBundle`] to be quick, so we take
    /// this opportunity to clean up the [`RenderBundleEncoder`]'s
    /// command stream and gather metadata about it that will help
    /// keep [`ExecuteBundle`] simple and fast. We remove redundant
    /// commands (along with their side data), note resource usage,
    /// and accumulate buffer and texture initialization actions.
    ///
    /// [`ExecuteBundle`]: RenderCommand::ExecuteBundle
    pub(crate) fn finish<A: HalApi, G: GlobalIdentityHandlerFactory>(
        self,
        desc: &RenderBundleDescriptor,
        device: &Device<A>,
        hub: &Hub<A, G>,
        token: &mut Token<Device<A>>,
    ) -> Result<RenderBundle<A>, RenderBundleError> {
        let (pipeline_layout_guard, mut token) = hub.pipeline_layouts.read(token);
        let (bind_group_guard, mut token) = hub.bind_groups.read(&mut token);
        let (pipeline_guard, mut token) = hub.render_pipelines.read(&mut token);
        let (query_set_guard, mut token) = hub.query_sets.read(&mut token);
        let (buffer_guard, mut token) = hub.buffers.read(&mut token);
        let (texture_guard, _) = hub.textures.read(&mut token);

        let mut state = State {
            trackers: RenderBundleScope::new(
                &*buffer_guard,
                &*texture_guard,
                &*bind_group_guard,
                &*pipeline_guard,
                &*query_set_guard,
            ),
            index: IndexState::new(),
            vertex: (0..hal::MAX_VERTEX_BUFFERS)
                .map(|_| VertexState::new())
                .collect(),
            bind: (0..hal::MAX_BIND_GROUPS).map(|_| None).collect(),
            push_constant_ranges: PushConstantState::new(),
            flat_dynamic_offsets: Vec::new(),
            used_bind_groups: 0,
            pipeline: None,
        };
        let mut commands = Vec::new();
        let mut pipeline_layout_id = None::<id::Valid<id::PipelineLayoutId>>;
        let mut buffer_memory_init_actions = Vec::new();
        let mut texture_memory_init_actions = Vec::new();

        let base = self.base.as_ref();
        let mut next_dynamic_offset = 0;

        for &command in base.commands {
            match command {
                RenderCommand::SetBindGroup {
                    index,
                    num_dynamic_offsets,
                    bind_group_id,
                } => {
                    let scope = PassErrorScope::SetBindGroup(bind_group_id);

                    let bind_group: &binding_model::BindGroup<A> = state
                        .trackers
                        .bind_groups
                        .add_single(&*bind_group_guard, bind_group_id)
                        .ok_or(RenderCommandError::InvalidBindGroup(bind_group_id))
                        .map_pass_err(scope)?;
                    self.check_valid_to_use(bind_group.device_id.value)
                        .map_pass_err(scope)?;

                    let max_bind_groups = device.limits.max_bind_groups;
                    if (index as u32) >= max_bind_groups {
                        return Err(RenderCommandError::BindGroupIndexOutOfRange {
                            index,
                            max: max_bind_groups,
                        })
                        .map_pass_err(scope);
                    }

                    // Identify the next `num_dynamic_offsets` entries from `base.dynamic_offsets`.
                    let num_dynamic_offsets = num_dynamic_offsets as usize;
                    let offsets_range =
                        next_dynamic_offset..next_dynamic_offset + num_dynamic_offsets;
                    next_dynamic_offset = offsets_range.end;
                    let offsets = &base.dynamic_offsets[offsets_range.clone()];

                    if bind_group.dynamic_binding_info.len() != offsets.len() {
                        return Err(RenderCommandError::InvalidDynamicOffsetCount {
                            actual: offsets.len(),
                            expected: bind_group.dynamic_binding_info.len(),
                        })
                        .map_pass_err(scope);
                    }

                    // Check for misaligned offsets.
                    for (offset, info) in offsets
                        .iter()
                        .map(|offset| *offset as wgt::BufferAddress)
                        .zip(bind_group.dynamic_binding_info.iter())
                    {
                        let (alignment, limit_name) =
                            buffer_binding_type_alignment(&device.limits, info.binding_type);
                        if offset % alignment as u64 != 0 {
                            return Err(RenderCommandError::UnalignedBufferOffset(
                                offset, limit_name, alignment,
                            ))
                            .map_pass_err(scope);
                        }
                    }

                    buffer_memory_init_actions.extend_from_slice(&bind_group.used_buffer_ranges);
                    texture_memory_init_actions.extend_from_slice(&bind_group.used_texture_ranges);

                    state.set_bind_group(index, bind_group_id, bind_group.layout_id, offsets_range);
                    unsafe {
                        state
                            .trackers
                            .merge_bind_group(&*texture_guard, &bind_group.used)
                            .map_pass_err(scope)?
                    };
                    //Note: stateless trackers are not merged: the lifetime reference
                    // is held to the bind group itself.
                }
                RenderCommand::SetPipeline(pipeline_id) => {
                    let scope = PassErrorScope::SetPipelineRender(pipeline_id);

                    state.pipeline = Some(pipeline_id);

                    let pipeline: &pipeline::RenderPipeline<A> = state
                        .trackers
                        .render_pipelines
                        .add_single(&*pipeline_guard, pipeline_id)
                        .ok_or(RenderCommandError::InvalidPipeline(pipeline_id))
                        .map_pass_err(scope)?;
                    self.check_valid_to_use(pipeline.device_id.value)
                        .map_pass_err(scope)?;

                    self.context
                        .check_compatible(&pipeline.pass_context)
                        .map_err(RenderCommandError::IncompatiblePipelineTargets)
                        .map_pass_err(scope)?;

                    if pipeline.flags.contains(PipelineFlags::WRITES_DEPTH_STENCIL)
                        && self.is_ds_read_only
                    {
                        return Err(RenderCommandError::IncompatiblePipelineRods)
                            .map_pass_err(scope);
                    }

                    let layout = &pipeline_layout_guard[pipeline.layout_id.value];
                    pipeline_layout_id = Some(pipeline.layout_id.value);

                    state.set_pipeline(
                        pipeline.strip_index_format,
                        &pipeline.vertex_strides,
                        &layout.bind_group_layout_ids,
                        &layout.push_constant_ranges,
                    );
                    commands.push(command);
                    if let Some(iter) = state.flush_push_constants() {
                        commands.extend(iter)
                    }
                }
                RenderCommand::SetIndexBuffer {
                    buffer_id,
                    index_format,
                    offset,
                    size,
                } => {
                    let scope = PassErrorScope::SetIndexBuffer(buffer_id);
                    let buffer: &resource::Buffer<A> = state
                        .trackers
                        .buffers
                        .merge_single(&*buffer_guard, buffer_id, hal::BufferUses::INDEX)
                        .map_pass_err(scope)?;
                    self.check_valid_to_use(buffer.device_id.value)
                        .map_pass_err(scope)?;
                    check_buffer_usage(buffer.usage, wgt::BufferUsages::INDEX)
                        .map_pass_err(scope)?;

                    let end = match size {
                        Some(s) => offset + s.get(),
                        None => buffer.size,
                    };
                    buffer_memory_init_actions.extend(buffer.initialization_status.create_action(
                        buffer_id,
                        offset..end,
                        MemoryInitKind::NeedsInitializedMemory,
                    ));
                    state.index.set_format(index_format);
                    state.index.set_buffer(buffer_id, offset..end);
                }
                RenderCommand::SetVertexBuffer {
                    slot,
                    buffer_id,
                    offset,
                    size,
                } => {
                    let scope = PassErrorScope::SetVertexBuffer(buffer_id);
                    let buffer: &resource::Buffer<A> = state
                        .trackers
                        .buffers
                        .merge_single(&*buffer_guard, buffer_id, hal::BufferUses::VERTEX)
                        .map_pass_err(scope)?;
                    self.check_valid_to_use(buffer.device_id.value)
                        .map_pass_err(scope)?;
                    check_buffer_usage(buffer.usage, wgt::BufferUsages::VERTEX)
                        .map_pass_err(scope)?;

                    let end = match size {
                        Some(s) => offset + s.get(),
                        None => buffer.size,
                    };
                    buffer_memory_init_actions.extend(buffer.initialization_status.create_action(
                        buffer_id,
                        offset..end,
                        MemoryInitKind::NeedsInitializedMemory,
                    ));
                    state.vertex[slot as usize].set_buffer(buffer_id, offset..end);
                }
                RenderCommand::SetPushConstant {
                    stages,
                    offset,
                    size_bytes,
                    values_offset: _,
                } => {
                    let scope = PassErrorScope::SetPushConstant;
                    let end_offset = offset + size_bytes;

                    let pipeline_layout_id = pipeline_layout_id
                        .ok_or(DrawError::MissingPipeline)
                        .map_pass_err(scope)?;
                    let pipeline_layout = &pipeline_layout_guard[pipeline_layout_id];

                    pipeline_layout
                        .validate_push_constant_ranges(stages, offset, end_offset)
                        .map_pass_err(scope)?;

                    commands.push(command);
                }
                RenderCommand::Draw {
                    vertex_count,
                    instance_count,
                    first_vertex,
                    first_instance,
                } => {
                    let scope = PassErrorScope::Draw {
                        indexed: false,
                        indirect: false,
                        pipeline: state.pipeline,
                    };
                    let vertex_limits = state.vertex_limits();
                    let last_vertex = first_vertex + vertex_count;
                    if last_vertex > vertex_limits.vertex_limit {
                        return Err(DrawError::VertexBeyondLimit {
                            last_vertex,
                            vertex_limit: vertex_limits.vertex_limit,
                            slot: vertex_limits.vertex_limit_slot,
                        })
                        .map_pass_err(scope);
                    }
                    let last_instance = first_instance + instance_count;
                    if last_instance > vertex_limits.instance_limit {
                        return Err(DrawError::InstanceBeyondLimit {
                            last_instance,
                            instance_limit: vertex_limits.instance_limit,
                            slot: vertex_limits.instance_limit_slot,
                        })
                        .map_pass_err(scope);
                    }
                    commands.extend(state.flush_vertices());
                    commands.extend(state.flush_binds(base.dynamic_offsets));
                    commands.push(command);
                }
                RenderCommand::DrawIndexed {
                    index_count,
                    instance_count,
                    first_index,
                    base_vertex: _,
                    first_instance,
                } => {
                    let scope = PassErrorScope::Draw {
                        indexed: true,
                        indirect: false,
                        pipeline: state.pipeline,
                    };
                    //TODO: validate that base_vertex + max_index() is within the provided range
                    let vertex_limits = state.vertex_limits();
                    let index_limit = state.index.limit();
                    let last_index = first_index + index_count;
                    if last_index > index_limit {
                        return Err(DrawError::IndexBeyondLimit {
                            last_index,
                            index_limit,
                        })
                        .map_pass_err(scope);
                    }
                    let last_instance = first_instance + instance_count;
                    if last_instance > vertex_limits.instance_limit {
                        return Err(DrawError::InstanceBeyondLimit {
                            last_instance,
                            instance_limit: vertex_limits.instance_limit,
                            slot: vertex_limits.instance_limit_slot,
                        })
                        .map_pass_err(scope);
                    }
                    commands.extend(state.index.flush());
                    commands.extend(state.flush_vertices());
                    commands.extend(state.flush_binds(base.dynamic_offsets));
                    commands.push(command);
                }
                RenderCommand::MultiDrawIndirect {
                    buffer_id,
                    offset,
                    count: None,
                    indexed: false,
                } => {
                    let scope = PassErrorScope::Draw {
                        indexed: false,
                        indirect: true,
                        pipeline: state.pipeline,
                    };
                    device
                        .require_downlevel_flags(wgt::DownlevelFlags::INDIRECT_EXECUTION)
                        .map_pass_err(scope)?;

                    let buffer: &resource::Buffer<A> = state
                        .trackers
                        .buffers
                        .merge_single(&*buffer_guard, buffer_id, hal::BufferUses::INDIRECT)
                        .map_pass_err(scope)?;
                    self.check_valid_to_use(buffer.device_id.value)
                        .map_pass_err(scope)?;
                    check_buffer_usage(buffer.usage, wgt::BufferUsages::INDIRECT)
                        .map_pass_err(scope)?;

                    buffer_memory_init_actions.extend(buffer.initialization_status.create_action(
                        buffer_id,
                        offset..(offset + mem::size_of::<wgt::DrawIndirectArgs>() as u64),
                        MemoryInitKind::NeedsInitializedMemory,
                    ));

                    commands.extend(state.flush_vertices());
                    commands.extend(state.flush_binds(base.dynamic_offsets));
                    commands.push(command);
                }
                RenderCommand::MultiDrawIndirect {
                    buffer_id,
                    offset,
                    count: None,
                    indexed: true,
                } => {
                    let scope = PassErrorScope::Draw {
                        indexed: true,
                        indirect: true,
                        pipeline: state.pipeline,
                    };
                    device
                        .require_downlevel_flags(wgt::DownlevelFlags::INDIRECT_EXECUTION)
                        .map_pass_err(scope)?;

                    let buffer: &resource::Buffer<A> = state
                        .trackers
                        .buffers
                        .merge_single(&*buffer_guard, buffer_id, hal::BufferUses::INDIRECT)
                        .map_pass_err(scope)?;
                    self.check_valid_to_use(buffer.device_id.value)
                        .map_pass_err(scope)?;
                    check_buffer_usage(buffer.usage, wgt::BufferUsages::INDIRECT)
                        .map_pass_err(scope)?;

                    buffer_memory_init_actions.extend(buffer.initialization_status.create_action(
                        buffer_id,
                        offset..(offset + mem::size_of::<wgt::DrawIndirectArgs>() as u64),
                        MemoryInitKind::NeedsInitializedMemory,
                    ));

                    commands.extend(state.index.flush());
                    commands.extend(state.flush_vertices());
                    commands.extend(state.flush_binds(base.dynamic_offsets));
                    commands.push(command);
                }
                RenderCommand::MultiDrawIndirect { .. }
                | RenderCommand::MultiDrawIndirectCount { .. } => unimplemented!(),
                RenderCommand::PushDebugGroup { color: _, len: _ } => unimplemented!(),
                RenderCommand::InsertDebugMarker { color: _, len: _ } => unimplemented!(),
                RenderCommand::PopDebugGroup => unimplemented!(),
                RenderCommand::WriteTimestamp { .. }
                | RenderCommand::BeginPipelineStatisticsQuery { .. }
                | RenderCommand::EndPipelineStatisticsQuery => unimplemented!(),
                RenderCommand::ExecuteBundle(_)
                | RenderCommand::SetBlendConstant(_)
                | RenderCommand::SetStencilReference(_)
                | RenderCommand::SetViewport { .. }
                | RenderCommand::SetScissor(_) => unreachable!("not supported by a render bundle"),
            }
        }

        Ok(RenderBundle {
            base: BasePass {
                label: desc.label.as_ref().map(|cow| cow.to_string()),
                commands,
                dynamic_offsets: state.flat_dynamic_offsets,
                string_data: Vec::new(),
                push_constant_data: Vec::new(),
            },
            is_ds_read_only: self.is_ds_read_only,
            device_id: Stored {
                value: id::Valid(self.parent_id),
                ref_count: device.life_guard.add_ref(),
            },
            used: state.trackers,
            buffer_memory_init_actions,
            texture_memory_init_actions,
            context: self.context,
            life_guard: LifeGuard::new(desc.label.borrow_or_default()),
        })
    }

    fn check_valid_to_use(
        &self,
        device_id: id::Valid<id::DeviceId>,
    ) -> Result<(), RenderBundleErrorInner> {
        if device_id.0 != self.parent_id {
            return Err(RenderBundleErrorInner::NotValidToUse);
        }

        Ok(())
    }

    pub fn set_index_buffer(
        &mut self,
        buffer_id: id::BufferId,
        index_format: wgt::IndexFormat,
        offset: wgt::BufferAddress,
        size: Option<wgt::BufferSize>,
    ) {
        self.base.commands.push(RenderCommand::SetIndexBuffer {
            buffer_id,
            index_format,
            offset,
            size,
        });
    }
}

/// Error type returned from `RenderBundleEncoder::new` if the sample count is invalid.
#[derive(Clone, Debug, Error)]
pub enum CreateRenderBundleError {
    #[error("invalid number of samples {0}")]
    InvalidSampleCount(u32),
    #[error("number of color attachments exceeds the limit")]
    TooManyColorAttachments,
}

/// Error type returned from `RenderBundleEncoder::new` if the sample count is invalid.
#[derive(Clone, Debug, Error)]
pub enum ExecutionError {
    #[error("buffer {0:?} is destroyed")]
    DestroyedBuffer(id::BufferId),
    #[error("using {0} in a render bundle is not implemented")]
    Unimplemented(&'static str),
}
impl PrettyError for ExecutionError {
    fn fmt_pretty(&self, fmt: &mut ErrorFormatter) {
        fmt.error(self);
        match *self {
            Self::DestroyedBuffer(id) => {
                fmt.buffer_label(&id);
            }
            Self::Unimplemented(_reason) => {}
        };
    }
}

pub type RenderBundleDescriptor<'a> = wgt::RenderBundleDescriptor<Label<'a>>;

//Note: here, `RenderBundle` is just wrapping a raw stream of render commands.
// The plan is to back it by an actual Vulkan secondary buffer, D3D12 Bundle,
// or Metal indirect command buffer.
pub struct RenderBundle<A: HalApi> {
    // Normalized command stream. It can be executed verbatim,
    // without re-binding anything on the pipeline change.
    base: BasePass<RenderCommand>,
    pub(super) is_ds_read_only: bool,
    pub(crate) device_id: Stored<id::DeviceId>,
    pub(crate) used: RenderBundleScope<A>,
    pub(super) buffer_memory_init_actions: Vec<BufferInitTrackerAction>,
    pub(super) texture_memory_init_actions: Vec<TextureInitTrackerAction>,
    pub(super) context: RenderPassContext,
    pub(crate) life_guard: LifeGuard,
}

unsafe impl<A: HalApi> Send for RenderBundle<A> {}
unsafe impl<A: HalApi> Sync for RenderBundle<A> {}

impl<A: HalApi> RenderBundle<A> {
    /// Actually encode the contents into a native command buffer.
    ///
    /// This is partially duplicating the logic of `command_encoder_run_render_pass`.
    /// However the point of this function is to be lighter, since we already had
    /// a chance to go through the commands in `render_bundle_encoder_finish`.
    ///
    /// Note that the function isn't expected to fail, generally.
    /// All the validation has already been done by this point.
    /// The only failure condition is if some of the used buffers are destroyed.
    pub(super) unsafe fn execute(
        &self,
        raw: &mut A::CommandEncoder,
        pipeline_layout_guard: &Storage<
            crate::binding_model::PipelineLayout<A>,
            id::PipelineLayoutId,
        >,
        bind_group_guard: &Storage<crate::binding_model::BindGroup<A>, id::BindGroupId>,
        pipeline_guard: &Storage<crate::pipeline::RenderPipeline<A>, id::RenderPipelineId>,
        buffer_guard: &Storage<crate::resource::Buffer<A>, id::BufferId>,
    ) -> Result<(), ExecutionError> {
        let mut offsets = self.base.dynamic_offsets.as_slice();
        let mut pipeline_layout_id = None::<id::Valid<id::PipelineLayoutId>>;
        if let Some(ref label) = self.base.label {
            raw.begin_debug_marker(label);
        }

        for command in self.base.commands.iter() {
            match *command {
                RenderCommand::SetBindGroup {
                    index,
                    num_dynamic_offsets,
                    bind_group_id,
                } => {
                    let bind_group = bind_group_guard.get(bind_group_id).unwrap();
                    raw.set_bind_group(
                        &pipeline_layout_guard[pipeline_layout_id.unwrap()].raw,
                        index as u32,
                        &bind_group.raw,
                        &offsets[..num_dynamic_offsets as usize],
                    );
                    offsets = &offsets[num_dynamic_offsets as usize..];
                }
                RenderCommand::SetPipeline(pipeline_id) => {
                    let pipeline = pipeline_guard.get(pipeline_id).unwrap();
                    raw.set_render_pipeline(&pipeline.raw);

                    pipeline_layout_id = Some(pipeline.layout_id.value);
                }
                RenderCommand::SetIndexBuffer {
                    buffer_id,
                    index_format,
                    offset,
                    size,
                } => {
                    let buffer = buffer_guard
                        .get(buffer_id)
                        .unwrap()
                        .raw
                        .as_ref()
                        .ok_or(ExecutionError::DestroyedBuffer(buffer_id))?;
                    let bb = hal::BufferBinding {
                        buffer,
                        offset,
                        size,
                    };
                    raw.set_index_buffer(bb, index_format);
                }
                RenderCommand::SetVertexBuffer {
                    slot,
                    buffer_id,
                    offset,
                    size,
                } => {
                    let buffer = buffer_guard
                        .get(buffer_id)
                        .unwrap()
                        .raw
                        .as_ref()
                        .ok_or(ExecutionError::DestroyedBuffer(buffer_id))?;
                    let bb = hal::BufferBinding {
                        buffer,
                        offset,
                        size,
                    };
                    raw.set_vertex_buffer(slot, bb);
                }
                RenderCommand::SetPushConstant {
                    stages,
                    offset,
                    size_bytes,
                    values_offset,
                } => {
                    let pipeline_layout_id = pipeline_layout_id.unwrap();
                    let pipeline_layout = &pipeline_layout_guard[pipeline_layout_id];

                    if let Some(values_offset) = values_offset {
                        let values_end_offset =
                            (values_offset + size_bytes / wgt::PUSH_CONSTANT_ALIGNMENT) as usize;
                        let data_slice = &self.base.push_constant_data
                            [(values_offset as usize)..values_end_offset];

                        raw.set_push_constants(&pipeline_layout.raw, stages, offset, data_slice)
                    } else {
                        super::push_constant_clear(
                            offset,
                            size_bytes,
                            |clear_offset, clear_data| {
                                raw.set_push_constants(
                                    &pipeline_layout.raw,
                                    stages,
                                    clear_offset,
                                    clear_data,
                                );
                            },
                        );
                    }
                }
                RenderCommand::Draw {
                    vertex_count,
                    instance_count,
                    first_vertex,
                    first_instance,
                } => {
                    raw.draw(first_vertex, vertex_count, first_instance, instance_count);
                }
                RenderCommand::DrawIndexed {
                    index_count,
                    instance_count,
                    first_index,
                    base_vertex,
                    first_instance,
                } => {
                    raw.draw_indexed(
                        first_index,
                        index_count,
                        base_vertex,
                        first_instance,
                        instance_count,
                    );
                }
                RenderCommand::MultiDrawIndirect {
                    buffer_id,
                    offset,
                    count: None,
                    indexed: false,
                } => {
                    let buffer = buffer_guard
                        .get(buffer_id)
                        .unwrap()
                        .raw
                        .as_ref()
                        .ok_or(ExecutionError::DestroyedBuffer(buffer_id))?;
                    raw.draw_indirect(buffer, offset, 1);
                }
                RenderCommand::MultiDrawIndirect {
                    buffer_id,
                    offset,
                    count: None,
                    indexed: true,
                } => {
                    let buffer = buffer_guard
                        .get(buffer_id)
                        .unwrap()
                        .raw
                        .as_ref()
                        .ok_or(ExecutionError::DestroyedBuffer(buffer_id))?;
                    raw.draw_indexed_indirect(buffer, offset, 1);
                }
                RenderCommand::MultiDrawIndirect { .. }
                | RenderCommand::MultiDrawIndirectCount { .. } => {
                    return Err(ExecutionError::Unimplemented("multi-draw-indirect"))
                }
                RenderCommand::PushDebugGroup { .. }
                | RenderCommand::InsertDebugMarker { .. }
                | RenderCommand::PopDebugGroup => {
                    return Err(ExecutionError::Unimplemented("debug-markers"))
                }
                RenderCommand::WriteTimestamp { .. }
                | RenderCommand::BeginPipelineStatisticsQuery { .. }
                | RenderCommand::EndPipelineStatisticsQuery => {
                    return Err(ExecutionError::Unimplemented("queries"))
                }
                RenderCommand::ExecuteBundle(_)
                | RenderCommand::SetBlendConstant(_)
                | RenderCommand::SetStencilReference(_)
                | RenderCommand::SetViewport { .. }
                | RenderCommand::SetScissor(_) => unreachable!(),
            }
        }

        if let Some(_) = self.base.label {
            raw.end_debug_marker();
        }

        Ok(())
    }
}

impl<A: HalApi> Resource for RenderBundle<A> {
    const TYPE: &'static str = "RenderBundle";

    fn life_guard(&self) -> &LifeGuard {
        &self.life_guard
    }
}

/// A render bundle's current index buffer state.
///
/// [`RenderBundleEncoder::finish`] uses this to drop redundant
/// `SetIndexBuffer` commands from the final [`RenderBundle`]. It
/// records index buffer state changes here, and then calls this
/// type's [`flush`] method before any indexed draw command to produce
/// a `SetIndexBuffer` command if one is necessary.
///
/// [`flush`]: IndexState::flush
#[derive(Debug)]
struct IndexState {
    buffer: Option<id::BufferId>,
    format: wgt::IndexFormat,
    pipeline_format: Option<wgt::IndexFormat>,
    range: Range<wgt::BufferAddress>,
    is_dirty: bool,
}

impl IndexState {
    /// Return a fresh state: no index buffer has been set yet.
    fn new() -> Self {
        Self {
            buffer: None,
            format: wgt::IndexFormat::default(),
            pipeline_format: None,
            range: 0..0,
            is_dirty: false,
        }
    }

    /// Return the number of entries in the current index buffer.
    ///
    /// Panic if no index buffer has been set.
    fn limit(&self) -> u32 {
        assert!(self.buffer.is_some());
        let bytes_per_index = match self.format {
            wgt::IndexFormat::Uint16 => 2,
            wgt::IndexFormat::Uint32 => 4,
        };
        ((self.range.end - self.range.start) / bytes_per_index) as u32
    }

    /// Prepare for an indexed draw, producing a `SetIndexBuffer`
    /// command if necessary.
    fn flush(&mut self) -> Option<RenderCommand> {
        if self.is_dirty {
            self.is_dirty = false;
            Some(RenderCommand::SetIndexBuffer {
                buffer_id: self.buffer.unwrap(),
                index_format: self.format,
                offset: self.range.start,
                size: wgt::BufferSize::new(self.range.end - self.range.start),
            })
        } else {
            None
        }
    }

    /// Set the current index buffer's format.
    fn set_format(&mut self, format: wgt::IndexFormat) {
        if self.format != format {
            self.format = format;
            self.is_dirty = true;
        }
    }

    /// Set the current index buffer.
    fn set_buffer(&mut self, id: id::BufferId, range: Range<wgt::BufferAddress>) {
        self.buffer = Some(id);
        self.range = range;
        self.is_dirty = true;
    }
}

/// The state of a single vertex buffer slot during render bundle encoding.
///
/// [`RenderBundleEncoder::finish`] uses this to drop redundant
/// `SetVertexBuffer` commands from the final [`RenderBundle`]. It
/// records one vertex buffer slot's state changes here, and then
/// calls this type's [`flush`] method just before any draw command to
/// produce a `SetVertexBuffer` commands if one is necessary.
///
/// [`flush`]: IndexState::flush
#[derive(Debug)]
struct VertexState {
    buffer: Option<id::BufferId>,
    range: Range<wgt::BufferAddress>,
    stride: wgt::BufferAddress,
    rate: wgt::VertexStepMode,
    is_dirty: bool,
}

impl VertexState {
    /// Construct a fresh `VertexState`: no buffer has been set for
    /// this slot.
    fn new() -> Self {
        Self {
            buffer: None,
            range: 0..0,
            stride: 0,
            rate: wgt::VertexStepMode::Vertex,
            is_dirty: false,
        }
    }

    /// Set this slot's vertex buffer.
    fn set_buffer(&mut self, buffer_id: id::BufferId, range: Range<wgt::BufferAddress>) {
        self.buffer = Some(buffer_id);
        self.range = range;
        self.is_dirty = true;
    }

    /// Generate a `SetVertexBuffer` command for this slot, if necessary.
    ///
    /// `slot` is the index of the vertex buffer slot that `self` tracks.
    fn flush(&mut self, slot: u32) -> Option<RenderCommand> {
        if self.is_dirty {
            self.is_dirty = false;
            Some(RenderCommand::SetVertexBuffer {
                slot,
                buffer_id: self.buffer.unwrap(),
                offset: self.range.start,
                size: wgt::BufferSize::new(self.range.end - self.range.start),
            })
        } else {
            None
        }
    }
}

/// A bind group that has been set at a particular index during render bundle encoding.
#[derive(Debug)]
struct BindState {
    /// The id of the bind group set at this index.
    bind_group_id: id::BindGroupId,

    /// The layout of `group`.
    layout_id: id::Valid<id::BindGroupLayoutId>,

    /// The range of dynamic offsets for this bind group, in the original
    /// command stream's `BassPass::dynamic_offsets` array.
    dynamic_offsets: Range<usize>,

    /// True if this index's contents have been changed since the last time we
    /// generated a `SetBindGroup` command.
    is_dirty: bool,
}

#[derive(Debug)]
struct PushConstantState {
    ranges: ArrayVec<wgt::PushConstantRange, { SHADER_STAGE_COUNT }>,
    is_dirty: bool,
}
impl PushConstantState {
    fn new() -> Self {
        Self {
            ranges: ArrayVec::new(),
            is_dirty: false,
        }
    }

    fn set_push_constants(&mut self, new_ranges: &[wgt::PushConstantRange]) -> bool {
        if &*self.ranges != new_ranges {
            self.ranges = new_ranges.iter().cloned().collect();
            self.is_dirty = true;
            true
        } else {
            false
        }
    }
}

#[derive(Debug)]
struct VertexLimitState {
    /// Length of the shortest vertex rate vertex buffer
    vertex_limit: u32,
    /// Buffer slot which the shortest vertex rate vertex buffer is bound to
    vertex_limit_slot: u32,
    /// Length of the shortest instance rate vertex buffer
    instance_limit: u32,
    /// Buffer slot which the shortest instance rate vertex buffer is bound to
    instance_limit_slot: u32,
}

/// State for analyzing and cleaning up bundle command streams.
///
/// To minimize state updates, [`RenderBundleEncoder::finish`]
/// actually just applies commands like [`SetBindGroup`] and
/// [`SetIndexBuffer`] to the simulated state stored here, and then
/// calls the `flush_foo` methods before draw calls to produce the
/// update commands we actually need.
struct State<A: HalApi> {
    /// Resources used by this bundle. This will become [`RenderBundle::used`].
    trackers: RenderBundleScope<A>,

    /// The current index buffer. We flush this state before indexed
    /// draw commands.
    index: IndexState,

    /// The state of each vertex buffer slot.
    vertex: ArrayVec<VertexState, { hal::MAX_VERTEX_BUFFERS }>,

    /// The bind group set at each index, if any.
    bind: ArrayVec<Option<BindState>, { hal::MAX_BIND_GROUPS }>,

    push_constant_ranges: PushConstantState,

    /// Dynamic offset values used by the cleaned-up command sequence.
    ///
    /// This becomes the final [`RenderBundle`]'s [`BasePass`]'s
    /// [`dynamic_offsets`] list.
    ///
    /// [`dynamic_offsets`]: BasePass::dynamic_offsets
    flat_dynamic_offsets: Vec<wgt::DynamicOffset>,

    used_bind_groups: usize,
    pipeline: Option<id::RenderPipelineId>,
}

impl<A: HalApi> State<A> {
    fn vertex_limits(&self) -> VertexLimitState {
        let mut vert_state = VertexLimitState {
            vertex_limit: u32::MAX,
            vertex_limit_slot: 0,
            instance_limit: u32::MAX,
            instance_limit_slot: 0,
        };
        for (idx, vbs) in self.vertex.iter().enumerate() {
            if vbs.stride == 0 {
                continue;
            }
            let limit = ((vbs.range.end - vbs.range.start) / vbs.stride) as u32;
            match vbs.rate {
                wgt::VertexStepMode::Vertex => {
                    if limit < vert_state.vertex_limit {
                        vert_state.vertex_limit = limit;
                        vert_state.vertex_limit_slot = idx as _;
                    }
                }
                wgt::VertexStepMode::Instance => {
                    if limit < vert_state.instance_limit {
                        vert_state.instance_limit = limit;
                        vert_state.instance_limit_slot = idx as _;
                    }
                }
            }
        }
        vert_state
    }

    /// Mark all non-empty bind group table entries from `index` onwards as dirty.
    fn invalidate_group_from(&mut self, index: usize) {
        for contents in self.bind[index..].iter_mut().flatten() {
            contents.is_dirty = true;
        }
    }

    fn set_bind_group(
        &mut self,
        slot: u8,
        bind_group_id: id::BindGroupId,
        layout_id: id::Valid<id::BindGroupLayoutId>,
        dynamic_offsets: Range<usize>,
    ) {
        // If this call wouldn't actually change this index's state, we can
        // return early.  (If there are dynamic offsets, the range will always
        // be different.)
        if dynamic_offsets.is_empty() {
            if let Some(ref contents) = self.bind[slot as usize] {
                if contents.bind_group_id == bind_group_id {
                    return;
                }
            }
        }

        // Record the index's new state.
        self.bind[slot as usize] = Some(BindState {
            bind_group_id,
            layout_id,
            dynamic_offsets,
            is_dirty: true,
        });

        // Once we've changed the bind group at a particular index, all
        // subsequent indices need to be rewritten.
        self.invalidate_group_from(slot as usize + 1);
    }

    fn set_pipeline(
        &mut self,
        index_format: Option<wgt::IndexFormat>,
        vertex_strides: &[(wgt::BufferAddress, wgt::VertexStepMode)],
        layout_ids: &[id::Valid<id::BindGroupLayoutId>],
        push_constant_layouts: &[wgt::PushConstantRange],
    ) {
        self.index.pipeline_format = index_format;

        for (vs, &(stride, step_mode)) in self.vertex.iter_mut().zip(vertex_strides) {
            if vs.stride != stride || vs.rate != step_mode {
                vs.stride = stride;
                vs.rate = step_mode;
                vs.is_dirty = true;
            }
        }

        let push_constants_changed = self
            .push_constant_ranges
            .set_push_constants(push_constant_layouts);

        self.used_bind_groups = layout_ids.len();
        let invalid_from = if push_constants_changed {
            Some(0)
        } else {
            self.bind
                .iter()
                .zip(layout_ids)
                .position(|(entry, &layout_id)| match *entry {
                    Some(ref contents) => contents.layout_id != layout_id,
                    None => false,
                })
        };
        if let Some(slot) = invalid_from {
            self.invalidate_group_from(slot);
        }
    }

    fn flush_push_constants(&mut self) -> Option<impl Iterator<Item = RenderCommand>> {
        let is_dirty = self.push_constant_ranges.is_dirty;

        if is_dirty {
            let nonoverlapping_ranges =
                super::bind::compute_nonoverlapping_ranges(&self.push_constant_ranges.ranges);

            Some(
                nonoverlapping_ranges
                    .into_iter()
                    .map(|range| RenderCommand::SetPushConstant {
                        stages: range.stages,
                        offset: range.range.start,
                        size_bytes: range.range.end - range.range.start,
                        values_offset: None,
                    }),
            )
        } else {
            None
        }
    }

    fn flush_vertices(&mut self) -> impl Iterator<Item = RenderCommand> + '_ {
        self.vertex
            .iter_mut()
            .enumerate()
            .flat_map(|(i, vs)| vs.flush(i as u32))
    }

    /// Generate `SetBindGroup` commands for any bind groups that need to be updated.
    fn flush_binds(
        &mut self,
        dynamic_offsets: &[wgt::DynamicOffset],
    ) -> impl Iterator<Item = RenderCommand> + '_ {
        // Append each dirty bind group's dynamic offsets to `flat_dynamic_offsets`.
        for contents in self.bind[..self.used_bind_groups].iter().flatten() {
            if contents.is_dirty {
                self.flat_dynamic_offsets
                    .extend_from_slice(&dynamic_offsets[contents.dynamic_offsets.clone()]);
            }
        }

        // Then, generate `SetBindGroup` commands to update the dirty bind
        // groups. After this, all bind groups are clean.
        self.bind[..self.used_bind_groups]
            .iter_mut()
            .enumerate()
            .flat_map(|(i, entry)| {
                if let Some(ref mut contents) = *entry {
                    if contents.is_dirty {
                        contents.is_dirty = false;
                        let offsets = &contents.dynamic_offsets;
                        return Some(RenderCommand::SetBindGroup {
                            index: i as u8,
                            bind_group_id: contents.bind_group_id,
                            num_dynamic_offsets: (offsets.end - offsets.start) as u8,
                        });
                    }
                }
                None
            })
    }
}

/// Error encountered when finishing recording a render bundle.
#[derive(Clone, Debug, Error)]
pub(super) enum RenderBundleErrorInner {
    #[error("resource is not valid to use with this render bundle because the resource and the bundle come from different devices")]
    NotValidToUse,
    #[error(transparent)]
    Device(#[from] DeviceError),
    #[error(transparent)]
    RenderCommand(RenderCommandError),
    #[error(transparent)]
    Draw(#[from] DrawError),
    #[error(transparent)]
    MissingDownlevelFlags(#[from] MissingDownlevelFlags),
}

impl<T> From<T> for RenderBundleErrorInner
where
    T: Into<RenderCommandError>,
{
    fn from(t: T) -> Self {
        Self::RenderCommand(t.into())
    }
}

/// Error encountered when finishing recording a render bundle.
#[derive(Clone, Debug, Error)]
#[error("{scope}")]
pub struct RenderBundleError {
    pub scope: PassErrorScope,
    #[source]
    inner: RenderBundleErrorInner,
}

impl RenderBundleError {
    pub(crate) const INVALID_DEVICE: Self = RenderBundleError {
        scope: PassErrorScope::Bundle,
        inner: RenderBundleErrorInner::Device(DeviceError::Invalid),
    };
}
impl PrettyError for RenderBundleError {
    fn fmt_pretty(&self, fmt: &mut ErrorFormatter) {
        // This error is wrapper for the inner error,
        // but the scope has useful labels
        fmt.error(self);
        self.scope.fmt_pretty(fmt);
    }
}

impl<T, E> MapPassErr<T, RenderBundleError> for Result<T, E>
where
    E: Into<RenderBundleErrorInner>,
{
    fn map_pass_err(self, scope: PassErrorScope) -> Result<T, RenderBundleError> {
        self.map_err(|inner| RenderBundleError {
            scope,
            inner: inner.into(),
        })
    }
}

pub mod bundle_ffi {
    use super::{RenderBundleEncoder, RenderCommand};
    use crate::{id, RawString};
    use std::{convert::TryInto, slice};
    use wgt::{BufferAddress, BufferSize, DynamicOffset, IndexFormat};

    /// # Safety
    ///
    /// This function is unsafe as there is no guarantee that the given pointer is
    /// valid for `offset_length` elements.
    #[no_mangle]
    pub unsafe extern "C" fn wgpu_render_bundle_set_bind_group(
        bundle: &mut RenderBundleEncoder,
        index: u32,
        bind_group_id: id::BindGroupId,
        offsets: *const DynamicOffset,
        offset_length: usize,
    ) {
        let redundant = bundle.current_bind_groups.set_and_check_redundant(
            bind_group_id,
            index,
            &mut bundle.base.dynamic_offsets,
            offsets,
            offset_length,
        );

        if redundant {
            return;
        }

        bundle.base.commands.push(RenderCommand::SetBindGroup {
            index: index.try_into().unwrap(),
            num_dynamic_offsets: offset_length.try_into().unwrap(),
            bind_group_id,
        });
    }

    #[no_mangle]
    pub extern "C" fn wgpu_render_bundle_set_pipeline(
        bundle: &mut RenderBundleEncoder,
        pipeline_id: id::RenderPipelineId,
    ) {
        if bundle.current_pipeline.set_and_check_redundant(pipeline_id) {
            return;
        }

        bundle
            .base
            .commands
            .push(RenderCommand::SetPipeline(pipeline_id));
    }

    #[no_mangle]
    pub extern "C" fn wgpu_render_bundle_set_vertex_buffer(
        bundle: &mut RenderBundleEncoder,
        slot: u32,
        buffer_id: id::BufferId,
        offset: BufferAddress,
        size: Option<BufferSize>,
    ) {
        bundle.base.commands.push(RenderCommand::SetVertexBuffer {
            slot,
            buffer_id,
            offset,
            size,
        });
    }

    #[no_mangle]
    pub extern "C" fn wgpu_render_bundle_set_index_buffer(
        encoder: &mut RenderBundleEncoder,
        buffer: id::BufferId,
        index_format: IndexFormat,
        offset: BufferAddress,
        size: Option<BufferSize>,
    ) {
        encoder.set_index_buffer(buffer, index_format, offset, size);
    }

    /// # Safety
    ///
    /// This function is unsafe as there is no guarantee that the given pointer is
    /// valid for `data` elements.
    #[no_mangle]
    pub unsafe extern "C" fn wgpu_render_bundle_set_push_constants(
        pass: &mut RenderBundleEncoder,
        stages: wgt::ShaderStages,
        offset: u32,
        size_bytes: u32,
        data: *const u8,
    ) {
        assert_eq!(
            offset & (wgt::PUSH_CONSTANT_ALIGNMENT - 1),
            0,
            "Push constant offset must be aligned to 4 bytes."
        );
        assert_eq!(
            size_bytes & (wgt::PUSH_CONSTANT_ALIGNMENT - 1),
            0,
            "Push constant size must be aligned to 4 bytes."
        );
        let data_slice = slice::from_raw_parts(data, size_bytes as usize);
        let value_offset = pass.base.push_constant_data.len().try_into().expect(
            "Ran out of push constant space. Don't set 4gb of push constants per RenderBundle.",
        );

        pass.base.push_constant_data.extend(
            data_slice
                .chunks_exact(wgt::PUSH_CONSTANT_ALIGNMENT as usize)
                .map(|arr| u32::from_ne_bytes([arr[0], arr[1], arr[2], arr[3]])),
        );

        pass.base.commands.push(RenderCommand::SetPushConstant {
            stages,
            offset,
            size_bytes,
            values_offset: Some(value_offset),
        });
    }

    #[no_mangle]
    pub extern "C" fn wgpu_render_bundle_draw(
        bundle: &mut RenderBundleEncoder,
        vertex_count: u32,
        instance_count: u32,
        first_vertex: u32,
        first_instance: u32,
    ) {
        bundle.base.commands.push(RenderCommand::Draw {
            vertex_count,
            instance_count,
            first_vertex,
            first_instance,
        });
    }

    #[no_mangle]
    pub extern "C" fn wgpu_render_bundle_draw_indexed(
        bundle: &mut RenderBundleEncoder,
        index_count: u32,
        instance_count: u32,
        first_index: u32,
        base_vertex: i32,
        first_instance: u32,
    ) {
        bundle.base.commands.push(RenderCommand::DrawIndexed {
            index_count,
            instance_count,
            first_index,
            base_vertex,
            first_instance,
        });
    }

    #[no_mangle]
    pub extern "C" fn wgpu_render_bundle_draw_indirect(
        bundle: &mut RenderBundleEncoder,
        buffer_id: id::BufferId,
        offset: BufferAddress,
    ) {
        bundle.base.commands.push(RenderCommand::MultiDrawIndirect {
            buffer_id,
            offset,
            count: None,
            indexed: false,
        });
    }

    #[no_mangle]
    pub extern "C" fn wgpu_render_bundle_draw_indexed_indirect(
        bundle: &mut RenderBundleEncoder,
        buffer_id: id::BufferId,
        offset: BufferAddress,
    ) {
        bundle.base.commands.push(RenderCommand::MultiDrawIndirect {
            buffer_id,
            offset,
            count: None,
            indexed: true,
        });
    }

    /// # Safety
    ///
    /// This function is unsafe as there is no guarantee that the given `label`
    /// is a valid null-terminated string.
    #[no_mangle]
    pub unsafe extern "C" fn wgpu_render_bundle_push_debug_group(
        _bundle: &mut RenderBundleEncoder,
        _label: RawString,
    ) {
        //TODO
    }

    #[no_mangle]
    pub extern "C" fn wgpu_render_bundle_pop_debug_group(_bundle: &mut RenderBundleEncoder) {
        //TODO
    }

    /// # Safety
    ///
    /// This function is unsafe as there is no guarantee that the given `label`
    /// is a valid null-terminated string.
    #[no_mangle]
    pub unsafe extern "C" fn wgpu_render_bundle_insert_debug_marker(
        _bundle: &mut RenderBundleEncoder,
        _label: RawString,
    ) {
        //TODO
    }
}
