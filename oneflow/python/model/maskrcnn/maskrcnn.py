# must import get cfg before importing oneflow
from config import get_default_cfgs

import oneflow as flow

from backbone import Backbone
from rpn import RPNHead, RPNLoss, RPNProposal
from box_head import BoxHead
from mask_head import MaskHead
from blob_watcher import save_blob_watched, blob_watched, diff_blob_watched

import os
import numpy as np
import argparse
from datetime import datetime

parser = argparse.ArgumentParser()
parser.add_argument(
    "-c", "--config_file", default=None, type=str, help="yaml config file"
)
parser.add_argument(
    "-load", "--model_load_dir", type=str, default="", required=False
)
parser.add_argument(
    "-g", "--gpu_num_per_node", type=int, default=1, required=False
)
parser.add_argument(
    "-d",
    "--debug",
    type=bool,
    default=True,
    required=False,
    help="debug with random data generated by numpy",
)
parser.add_argument(
    "-rpn", "--rpn_only", default=False, action="store_true", required=False
)
parser.add_argument(
    "-md", "--mock_dataset", default=False, action="store_true", required=False
)
parser.add_argument(
    "-mp",
    "--mock_dataset_path",
    type=str,
    default="/tmp/shared_with_zwx/mock_data_600x1000_b2.pkl",
    required=False,
)
parser.add_argument(
    "-eval", "--eval", default=False, action="store_true", required=False
)
parser.add_argument(
    "-td",
    "--train_with_real_dataset",
    default=False,
    action="store_true",
    required=False,
)
parser.add_argument(
    "-cp", "--ctrl_port", type=int, default=19765, required=False
)
parser.add_argument(
    "-save",
    "--model_save_dir",
    type=str,
    default="./model_save-{}".format(
        str(datetime.now().strftime("%Y-%m-%d--%H-%M-%S"))
    ),
    required=False,
)
parser.add_argument(
    "-v", "--verbose", default=False, action="store_true", required=False
)
parser.add_argument("-i", "--iter_num", type=int, default=10, required=False)
parser.add_argument(
    "-lr", "--primary_lr", type=float, default=0.02, required=False
)
terminal_args = parser.parse_args()


debug_data = None

if terminal_args.mock_dataset:
    from mock_data import MockData

    debug_data = MockData(terminal_args.mock_dataset_path, 64)


def get_numpy_placeholders():
    import numpy as np

    (N, H, W, C) = (2, 64, 64, 3)
    R = 50
    G = 12
    return {
        "images": np.random.randn(N, H, W, C).astype(np.float32),
        "image_sizes": np.random.randn(N, 2).astype(np.int32),
        "gt_boxes": np.random.randn(N, R, 4).astype(np.float32),
        "gt_segms": np.random.randn(N, G, 28, 28).astype(np.int8),
        "gt_labels": np.random.randn(N, G).astype(np.int32),
        "rpn_proposals": np.random.randn(2000, 4).astype(np.float32),
        "detections": np.random.randn(2000, 4).astype(np.float32),
        "fpn_feature_map1": np.random.randn(1, 256, 512, 512).astype(
            np.float32
        ),
        "fpn_feature_map2": np.random.randn(1, 256, 256, 256).astype(
            np.float32
        ),
        "fpn_feature_map3": np.random.randn(1, 256, 128, 128).astype(
            np.float32
        ),
        "fpn_feature_map4": np.random.randn(1, 256, 64, 64).astype(np.float32),
    }


placeholders = get_numpy_placeholders()


def maskrcnn_train(images, image_sizes, gt_boxes, gt_segms, gt_labels):
    r"""Mask-RCNN
    Args:
    images: (N, H, W, C)
    image_sizes: (N, 2)
    gt_boxes: (N, R, 4), dynamic
    gt_segms: (N, G, 28, 28), dynamic
    gt_labels: (N, G), dynamic
    """
    assert images.is_dynamic is True
    assert images.shape[3] == 3
    assert image_sizes.is_dynamic is False
    assert gt_boxes.num_of_lod_levels == 2
    # if it is mask target projected, num_of_lod_levels is 0
    if gt_segms.num_of_lod_levels == 0:
        assert gt_segms.is_dynamic is True
    else:
        assert gt_segms.num_of_lod_levels == 2
    assert gt_labels.num_of_lod_levels == 2
    cfg = get_default_cfgs()
    if terminal_args.config_file is not None:
        cfg.merge_from_file(terminal_args.config_file)
        print("merged config from {}".format(terminal_args.config_file))
    cfg.freeze()
    if terminal_args.verbose:
        print(cfg)

    backbone = Backbone(cfg)
    rpn_head = RPNHead(cfg)
    rpn_loss = RPNLoss(cfg)
    rpn_proposal = RPNProposal(cfg)
    box_head = BoxHead(cfg)
    mask_head = MaskHead(cfg)

    image_size_list = [
        flow.squeeze(
            flow.local_gather(image_sizes, flow.constant(i, dtype=flow.int32)),
            [0],
        )
        for i in range(image_sizes.shape[0])
    ]
    gt_boxes_list = flow.piece_slice(gt_boxes, cfg.TRAINING_CONF.IMG_PER_GPU, name="piece_gt_boxes")
    gt_labels_list = flow.piece_slice(gt_labels, cfg.TRAINING_CONF.IMG_PER_GPU, name="piece_slice_gt_labels")
    gt_segms_list = None
    if gt_segms.num_of_lod_levels == 2:
        gt_segms_list = flow.piece_slice(
            gt_segms, cfg.TRAINING_CONF.IMG_PER_GPU, name="piece_slice_gt_segms"
        )
    else:
        gt_segms_list = gt_segms
    anchors = []
    for i in range(cfg.DECODER.FPN_LAYERS):
        anchors.append(
            flow.detection.anchor_generate(
                images=images,
                feature_map_stride=cfg.DECODER.FEATURE_MAP_STRIDE * pow(2, i),
                aspect_ratios=cfg.DECODER.ASPECT_RATIOS,
                anchor_scales=cfg.DECODER.ANCHOR_SCALES * pow(2, i),
            )
        )

    # Backbone
    # CHECK_POINT: fpn features
    # with flow.watch_scope(blob_watcher=blob_watched, diff_blob_watcher=diff_blob_watched):
    features = backbone.build(flow.transpose(images, perm=[0, 3, 1, 2]))

    # RPN
    # with flow.watch_scope(blob_watcher=blob_watched, diff_blob_watcher=diff_blob_watched):
    cls_logit_list, bbox_pred_list = rpn_head.build(features)
    rpn_bbox_loss, rpn_objectness_loss = rpn_loss.build(
        anchors, image_size_list, gt_boxes_list, bbox_pred_list, cls_logit_list
    )
    if terminal_args.rpn_only:
        return rpn_bbox_loss, rpn_objectness_loss

    proposals = rpn_proposal.build(
        anchors, cls_logit_list, bbox_pred_list, image_size_list, gt_boxes_list
    )

    # with flow.watch_scope(blob_watcher=blob_watched, diff_blob_watcher=diff_blob_watched), flow.watch_scope(blob_watcher=MakeWatcherCallback("forward"), diff_blob_watcher=MakeWatcherCallback("backward")):
    # Box Head
    box_loss, cls_loss, pos_proposal_list, pos_gt_indices_list = box_head.build_train(
        proposals, gt_boxes_list, gt_labels_list, features
    )

    # Mask Head
    mask_loss = mask_head.build_train(
        pos_proposal_list,
        pos_gt_indices_list,
        gt_segms_list,
        gt_labels_list,
        features,
    )

    return rpn_bbox_loss, rpn_objectness_loss, box_loss, cls_loss, mask_loss

def MakeWatcherCallback(prompt):
    def Callback(blob, blob_def):
        if prompt == "forward":
            return
        print("%s, lbn: %s, min: %s, max: %s"
              %(prompt, blob_def.logical_blob_name, blob.min(), blob.max()))
    return Callback

def maskrcnn_eval(images, image_sizes):
    cfg = get_default_cfgs()
    if terminal_args.config_file is not None:
        cfg.merge_from_file(terminal_args.config_file)
    cfg.freeze()
    print(cfg)
    backbone = Backbone(cfg)
    rpn_head = RPNHead(cfg)
    rpn_proposal = RPNProposal(cfg)
    box_head = BoxHead(cfg)
    mask_head = MaskHead(cfg)

    image_size_list = [
        flow.squeeze(
            flow.local_gather(image_sizes, flow.constant(i, dtype=flow.int32)),
            [0],
        )
        for i in range(image_sizes.shape[0])
    ]
    anchors = []
    for i in range(cfg.DECODER.FPN_LAYERS):
        anchors.append(
            flow.detection.anchor_generate(
                images=flow.transpose(images, perm=[0, 2, 3, 1]),
                feature_map_stride=cfg.DECODER.FEATURE_MAP_STRIDE * pow(2, i),
                aspect_ratios=cfg.DECODER.ASPECT_RATIOS,
                anchor_scales=cfg.DECODER.ANCHOR_SCALES * pow(2, i),
            )
        )

    # Backbone
    features = backbone.build(images)

    # RPN
    cls_logit_list, bbox_pred_list = rpn_head.build(features)
    rpn_proposals = rpn_proposal.build(
        anchors, cls_logit_list, bbox_pred_list, image_size_list, None
    )

    # Box Head
    cls_logits, box_regressions = box_head.build_eval(rpn_proposals, features)

    # Mask Head
    # TODO: get proposals from box_head post-processors
    # mask_logits = mask_head.build_eval(proposals, features)

    return tuple(rpn_proposals) + (cls_logits,) + (box_regressions,)


if terminal_args.mock_dataset:

    @flow.function
    def mock_train(
        images=debug_data.blob_def("images"),
        image_sizes=debug_data.blob_def("image_size"),
        gt_boxes=debug_data.blob_def("gt_bbox"),
        gt_segms=debug_data.blob_def("segm_mask_targets"),
        gt_labels=debug_data.blob_def("gt_labels"),
    ):
        flow.config.train.primary_lr(terminal_args.primary_lr)
        print("primary_lr:", terminal_args.primary_lr)
        flow.config.train.model_update_conf(dict(naive_conf={}))
        # TODO: distribute map only support remote blob for now, so identity is required here
        image_sizes = flow.identity(image_sizes)
        outputs = maskrcnn_train(
            flow.transpose(images, perm=[0, 2, 3, 1]),
            image_sizes,
            gt_boxes,
            gt_segms,
            gt_labels,
        )
        for loss in outputs:
            flow.losses.add_loss(loss)
        return outputs


if terminal_args.eval:

    @flow.function
    def maskrcnn_eval_job(
        images=flow.input_blob_def(
            (1, 3, 1280, 800), dtype=flow.float, is_dynamic=True
        ),
        image_sizes=flow.input_blob_def(
            (1, 2), dtype=flow.int32, is_dynamic=False
        ),
    ):
        return maskrcnn_eval(images, image_sizes)


if __name__ == "__main__":
    flow.config.gpu_device_num(terminal_args.gpu_num_per_node)
    flow.config.ctrl_port(terminal_args.ctrl_port)

    flow.config.default_data_type(flow.float)
    check_point = flow.train.CheckPoint()
    if not terminal_args.model_load_dir:
        check_point.init()
    else:
        check_point.load(terminal_args.model_load_dir)
    if terminal_args.debug:
        if terminal_args.eval:
            import numpy as np

            images = np.load(
                "/tmp/shared_with_jxf/maskrcnn_eval_input_data/images.npy"
            )
            image_sizes = np.load(
                "/tmp/shared_with_jxf/maskrcnn_eval_input_data/image_sizes.npy"
            )
            results = maskrcnn_eval_job(images, image_sizes).get()
            proposals, cls_logits, box_regressions = maskrcnn_eval_job(
                images, image_sizes
            ).get()
        elif terminal_args.mock_dataset:
            if terminal_args.rpn_only:
                print(
                    "{:>8} {:>16} {:>16}".format(
                        "iter", "rpn_bbox_loss", "rpn_obj_loss"
                    )
                )
            else:
                print(
                    "{:>8} {:>16} {:>16} {:>16} {:>16} {:>16}".format(
                        "iter",
                        "loss_rpn_box_reg",
                        "loss_objectness",
                        "loss_box_reg",
                        "loss_classifier",
                        "loss_mask",
                    )
                )
            for i in range(terminal_args.iter_num):

                def save_model():
                    return
                    if not os.path.exists(terminal_args.model_save_dir):
                        os.makedirs(terminal_args.model_save_dir)
                    model_dst = os.path.join(
                        terminal_args.model_save_dir, "iter-" + str(i)
                    )
                    print("saving models to {}".format(model_dst))
                    check_point.save(model_dst)

                if i == 0:
                    save_model()

                train_loss = mock_train(
                    debug_data.blob("images"),
                    debug_data.blob("image_size"),
                    debug_data.blob("gt_bbox"),
                    debug_data.blob("segm_mask_targets"),
                    debug_data.blob("gt_labels"),
                ).get()
                fmt_str = "{:>8} " + "{:>16.10f} " * len(train_loss)
                print_loss = [i]
                for loss in train_loss:
                    print_loss.append(loss.mean())
                print(fmt_str.format(*print_loss))
                save_blob_watched(i)

                if (i + 1) % 10 == 0:
                    save_model()
