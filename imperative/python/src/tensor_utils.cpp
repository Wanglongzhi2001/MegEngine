/**
 * \file imperative/python/src/tensor.cpp
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2021 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "megbrain/common.h"
#include "megbrain/dtype.h"
#include "megbrain/imperative/ops/autogen.h"
#include "megbrain/imperative/ops/backward_graph.h"
#include "megbrain/imperative/ops/utility.h"
#include "megbrain/imperative/profiler.h"
#include "megbrain/imperative/transformations/eval.h"
#include "megbrain/imperative/transformations/lazy.h"
#include "megbrain/imperative/transformations/scalar.h"
#include "megbrain/imperative/transformations/symbol.h"
#include "megbrain/imperative/transformations/trace.h"
#include "megbrain/imperative/utils/map.h"
#include "megbrain/imperative/utils/stats.h"
#include "megbrain/opr/io.h"
#include "megbrain/plugin/profiler.h"

#include "./common.h"
#include "./grad.h"
#include "./graph_rt.h"
#include "./helper.h"
#include "./module_trace.h"
#include "./numpy_dtypes.h"
#include "./tensor.h"
#include "./tensor_utils.h"
#include "./transformation.h"

#include <object.h>
#include <pybind11/numpy.h>
#include <pybind11/operators.h>
#include <pybind11/pytypes.h>
#include <pyerrors.h>
#include <range/v3/all.hpp>
#include <string>

#include <unordered_map>

#include "../../src/impl/mgb_cg_impl.h"

namespace py = pybind11;
namespace views = ranges::views;

namespace mgb::imperative::python {

/* ============== convert inputs ============== */

// map numpy.dtype.kind to priority
inline uint8_t category_priority(char c) {
    switch (c) {
        case 'f':
            return 3;  // floating-point
        case 'i':
            return 2;  // signed integer
        case 'u':
            return 2;  // unsigned integer
        case 'b':
            return 1;  // boolean
        default:
            return 0;
    }
}

// Returns the maximum value of the priority of each type in the list `types`.
uint8_t max_priority(SmallVector<PyArray_Descr*> types) {
    if (types.size() == 0) {
        return 0;
    } else {
        uint8_t max_p = 0;
        for (auto&& desc : types) {
            max_p = std::max(max_p, category_priority(desc->kind));
        }
        return max_p;
    }
}

// Returns the data type with sufficient size to hold all types of
// category `cat` in the list `types`.
PyArray_Descr* promote_types(SmallVector<PyArray_Descr*> types, uint8_t cat) {
    // Return value: New reference
    SmallVector<PyArray_Descr*> used_types;
    for (auto&& desc : types) {
        auto&& v = category_priority(desc->kind);
        if (v == cat) {
            used_types.emplace_back(desc);
        }
    }
    mgb_assert(used_types.size() > 0, "size of used_types is 0");
    PyArray_Descr* res = used_types[0];
    Py_INCREF(res);

    for (size_t i = 1; i < used_types.size(); ++i) {
        PyArray_Descr* tmp = PyArray_PromoteTypes(used_types[i], res);
        Py_DECREF(res);
        res = tmp;
    }
    return res;
}

PyArray_Descr* scalar2dtype(PyObject* arg) {
    // Return value: New reference
    if (PyBool_Check(arg)) {
        auto&& descr = PyArray_DescrFromType(NPY_BOOL);
        return descr;
    }
    if (PyLong_CheckExact(arg)) {
        auto&& descr = PyArray_DescrFromType(NPY_INT32);
        return descr;
    }
    if (PyFloat_CheckExact(arg)) {
        auto&& descr = PyArray_DescrFromType(NPY_FLOAT32);
        return descr;
    }
    return nullptr;
}

PyArray_Descr* _dtype_promotion(PyObject* const* args, size_t nargs) {
    // Return value: New reference
    SmallVector<PyArray_Descr*> tensors;
    SmallVector<PyArray_Descr*> scalars;

    bool is_tuple = false;
    PyObject* tuple = nullptr;
    if (nargs == 1 && (PyTuple_Check(args[0]) || PyList_Check(args[0]))) {
        if (PyList_Check(args[0])) {
            tuple = PyList_AsTuple(args[0]);
        } else {
            tuple = args[0];
            Py_INCREF(tuple);
        }
        nargs = PyTuple_Size(tuple);
        is_tuple = true;
    }

    for (size_t i = 0; i < nargs; ++i) {
        PyObject* handle = is_tuple ? PyTuple_GetItem(tuple, i) : args[i];
        if (handle == Py_None)
            continue;
        TensorWrapper* tw = TensorWrapper::try_cast(handle);
        if (tw) {
            mgb::DType type = tw->m_tensor->dtype();
            auto&& descr = npy::dtype_mgb2np_descr(type);
            Py_INCREF(descr.get());
            tensors.emplace_back(descr.get());
        } else {
            if (PyArray_Check(handle) || PyArray_CheckScalar(handle)) {
                auto&& descr = PyArray_DescrFromObject(handle, nullptr);
                tensors.emplace_back(descr);
                continue;
            }

            if (py::isinstance<PySymbolVar>(py::handle(handle))) {
                auto var = py::handle(handle).cast<PySymbolVar*>();
                mgb::DType type = var->m_node->dtype();
                auto&& descr = npy::dtype_mgb2np_descr(type);
                Py_INCREF(descr.get());
                tensors.emplace_back(descr.get());
                continue;
            }

            PyArray_Descr* descr = scalar2dtype(handle);
            if (descr) {
                scalars.emplace_back(descr);
                continue;
            }
        }
    }

    auto max_pri_scalars = max_priority(scalars);
    auto max_pri_tensors = max_priority(tensors);

    if (max_pri_scalars <= 0 && max_pri_tensors <= 0) {
        throw py::value_error("invalid input, no dtype avaliable");
    }
    PyArray_Descr* res;
    if (max_pri_scalars > max_pri_tensors) {
        res = promote_types(scalars, max_pri_scalars);
    } else {
        res = promote_types(tensors, max_pri_tensors);
    }
    for (auto* p : tensors) {
        Py_DECREF(p);
    }
    for (auto* p : scalars) {
        Py_DECREF(p);
    }
    Py_XDECREF(tuple);
    return res;
}

CompNode _get_device(PyObject* const* args, size_t nargs) {
    bool is_tuple = false;
    PyObject* tuple = nullptr;
    if (nargs == 1 && (PyTuple_Check(args[0]) || PyList_Check(args[0]))) {
        if (PyList_Check(args[0])) {
            tuple = PyList_AsTuple(args[0]);
        } else {
            tuple = args[0];
            Py_INCREF(tuple);
        }
        nargs = PyTuple_Size(tuple);
        is_tuple = true;
    }
    bool valid = false;
    CompNode cn;
    for (size_t i = 0; i < nargs; ++i) {
        PyObject* handle = is_tuple ? PyTuple_GetItem(tuple, i) : args[i];
        TensorWrapper* tw = TensorWrapper::try_cast(handle);

        bool is_symvar = py::isinstance<PySymbolVar>(py::handle(handle));
        if (tw || is_symvar) {
            if (!valid) {
                cn = tw ? tw->m_tensor->comp_node()
                        : py::handle(handle).cast<PySymbolVar*>()->m_node->comp_node();
                valid = true;
            } else {
                CompNode cn1 = tw ? tw->m_tensor->comp_node()
                                  : py::handle(handle)
                                               .cast<PySymbolVar*>()
                                               ->m_node->comp_node();
                if (cn1 != cn) {
                    throw py::value_error(ssprintf(
                            "ambiguous device: %s (from %s) vs %s (from %s)",
                            cn.to_string().c_str(), cn.to_string_logical().c_str(),
                            cn1.to_string().c_str(), cn1.to_string_logical().c_str()));
                }
            }
        }
    }
    if (!valid) {
        return CompNode::load(get_default_device());
    }
    Py_XDECREF(tuple);
    return cn;
}

// Returns the dtype that would result from performing an arithmetic
// operation on the provided input tensors and scalars.
PyObject* dtype_promotion(PyObject* self, PyObject* const* args, size_t nargs) {
    if (!nargs) {
        PyErr_SetString(PyExc_TypeError, "empty input is not allowed");
        return nullptr;
    }
    try {
        PyArray_Descr* res = _dtype_promotion(args, nargs);
        return py::cast(npy::dtype_np2mgb_descr(res)).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* get_device(PyObject* self, PyObject* const* args, size_t nargs) {
    if (!nargs) {
        PyErr_SetString(PyExc_TypeError, "empty input is not allowed");
        return nullptr;
    }
    try {
        CompNode cn = _get_device(args, nargs);
        return py::cast(cn).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

bool is_scalar(PyObject* tensor) {
    if (py::isinstance<PySymbolVar>(py::handle(tensor))) {
        auto var = py::handle(tensor).cast<PySymbolVar*>();
        return var->is_scalar;
    }
    auto* tw = TensorWrapper::try_cast(tensor);
    if (tw) {
        return tw->m_tensor->is_scalar();
    }
    return PyArray_CheckAnyScalar(tensor);
}

bool is_bool_list(PyObject* arg) {
    if (!PyList_Check(arg)) {
        return false;
    }
    size_t sz = PyList_Size(arg);
    if (!sz) {
        return false;
    }
    for (size_t i = 0; i < sz; ++i) {
        PyObject* handle = PyList_GetItem(arg, i);
        if (!PyBool_Check(handle)) {
            return false;
        }
    }
    return true;
}

bool is_bool_dtype(PyObject* args) {
    if (!PyObject_HasAttrString(args, "dtype"))
        return false;
    PyObject* dobj = PyObject_GetAttrString(args, "dtype");
    PyArray_Descr* dtype;
    PyArray_DescrConverter(dobj, &dtype);
    bool ret = (dtype->kind == 'b');
    Py_XDECREF(dtype);
    Py_XDECREF(dobj);
    return ret;
}

py::object _Const(
        py::handle value, py::handle dtype, py::handle device, py::handle ref_hdl) {
    py::object val = py::reinterpret_borrow<py::object>(value);
    if (PyArray_Check(value.ptr())) {
        py::tuple strides =
                py::reinterpret_borrow<py::tuple>(getattr(value, "strides"));
        bool need_squeeze = false;
        for (size_t i = 0; i < strides.size(); ++i) {
            if (strides[i].cast<ptrdiff_t>() == 0) {
                need_squeeze = true;
            }
        }
        if (need_squeeze) {
            val = py::reinterpret_borrow<py::array>(value);
            py::object orig_shp = val.attr("shape");
            val = val.attr("squeeze")();
            val = val.attr("reshape")(orig_shp);
        }
    }
    py::object ref;
    if (py::isinstance<py::tuple>(ref_hdl)) {
        py::tuple tup = py::reinterpret_borrow<py::tuple>(ref_hdl);
        if (tup.size()) {
            ref = tup[0];
        } else {
            ref = py::none();
        }
    } else {
        ref = py::reinterpret_borrow<py::object>(ref_hdl);
    }
    if (py::isinstance<PySymbolVar>(ref)) {
        auto ref_var = ref.cast<PySymbolVar*>();
        auto* graph = ref_var->m_node->owner_graph();
        CompNode cn;
        if (device.ptr() == Py_None) {
            cn = ref_var->m_node->comp_node();
        } else {
            cn = device.cast<CompNode>();
        }
        OperatorNodeConfig config(cn);
        auto hv = npy::np2tensor(
                val.ptr(), npy::Meth::borrow(cn), dtype.cast<mgb::DType>());
        auto typeobj = ref.get_type();
        return typeobj(opr::ImmutableTensor::make(*graph, hv, config).node());
    }
    py::object device_obj;
    if (device.ptr() == Py_None) {
        device_obj = py::cast(CompNode::load(get_default_device()));
    } else if (py::isinstance<py::str>(device)) {
        py::object dmap =
                getattr(py::reinterpret_borrow<py::object>((PyObject*)py_tensor_type),
                        "dmap_callback");
        if (dmap.ptr() != Py_None) {
            device_obj = dmap(device);
        } else {
            device_obj = py::cast(CompNode::load(device.cast<std::string>()));
        }
    } else if (py::isinstance<CompNode>(device)) {
        device_obj = py::reinterpret_borrow<py::object>(device);
    } else {
        device_obj = getattr(device, "_cn");
    }
    py::tuple tup = py::make_tuple(val, dtype, device_obj, true, false, py::none());
    return TensorWrapper::make(py_tensor_type, tup.ptr(), nullptr);
}

py::tuple _make_shape_tuple(py::handle shape) {
    py::list orig;
    py::list ret(0);
    auto solve_one = [&](py::handle val) {
        if (TensorWrapper::try_cast(val.ptr()) || py::isinstance<PySymbolVar>(val)) {
            py::object np = getattr(val, "numpy")();
            PyArrayObject* arr = (PyArrayObject*)np.ptr();
            PyObject* maybe_list = PyArray_ToList(arr);
            if (PyList_Check(maybe_list)) {
                py::list may = py::reinterpret_steal<py::list>(maybe_list);
                for (size_t i = 0; i < may.size(); ++i) {
                    ret.append(may[i]);
                }
            } else {
                mgb_assert(PyLong_Check(maybe_list));
                ret.append(PyLong_AsLong(maybe_list));
                Py_XDECREF(maybe_list);
            }
        } else if (PyArray_Check(val.ptr())) {
            ret.append(PyArray_PyIntAsInt(val.ptr()));
        } else {
            ret.append(PyLong_AsLong(val.ptr()));
        }
    };
    if (PyArray_Check(shape.ptr()) && !PyArray_CheckAnyScalar(shape.ptr())) {
        orig = py::reinterpret_steal<py::list>(
                PyArray_ToList((PyArrayObject*)shape.ptr()));
        for (size_t i = 0; i < orig.size(); ++i) {
            solve_one(orig[i]);
        }
    } else if (PyList_Check(shape.ptr())) {
        orig = py::reinterpret_borrow<py::list>(shape);
        for (size_t i = 0; i < orig.size(); ++i) {
            solve_one(orig[i]);
        }
    } else if (PyTuple_Check(shape.ptr())) {
        py::tuple tup = py::reinterpret_borrow<py::tuple>(shape);
        for (size_t i = 0; i < tup.size(); ++i) {
            solve_one(tup[i]);
        }
    } else {
        solve_one(shape);
    }
    return py::reinterpret_steal<py::tuple>(PyList_AsTuple(ret.ptr()));
}

py::object _get_index(py::object tensor, py::object src) {
    if (!TensorWrapper::try_cast(tensor.ptr()) &&
        !py::isinstance<PySymbolVar>(tensor)) {
        auto get_const = [&](mgb::DType dtype) -> py::object {
            return _Const(tensor, py::cast(dtype), src.attr("device"), src);
        };
        if (is_bool_list(tensor.ptr()) || is_bool_dtype(tensor.ptr())) {
            tensor = get_const(dtype::Bool());
        } else {
            tensor = get_const(dtype::Int32());
        }
        if (!is_bool_dtype(tensor.ptr())) {
            return tensor;
        }
    } else {
        if (!is_bool_dtype(tensor.ptr())) {
            return tensor;
        }
    }
    static std::shared_ptr<OpDef> op = CondTake::make();
    std::vector<PyObject*> p;
    p.resize(3);
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = tensor.ptr();
    p[2] = tensor.ptr();
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    return ret[1];
}

py::tuple _try_cond_take(py::handle tensor, py::handle index) {
    if (!hasattr(index, "dtype") || !hasattr(index, "shape")) {
        return py::tuple();
    }
    if (!is_bool_dtype(index.ptr()) ||
        _make_shape_tuple(getattr(index, "shape"))
                .not_equal(_make_shape_tuple(getattr(tensor, "shape")))) {
        return py::tuple();
    }
    py::object iobj;
    if (PyArray_Check(index.ptr())) {
        iobj =
                _Const(index, py::cast((mgb::DType)dtype::Bool()),
                       getattr(tensor, "device"), tensor);
    } else {
        iobj = py::reinterpret_borrow<py::object>(index);
    }
    static std::shared_ptr<OpDef> op = CondTake::make();
    std::vector<PyObject*> p;
    p.resize(3);
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = tensor.ptr();
    p[2] = iobj.ptr();
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    return ret;
}

py::tuple _remove_ellipsis(py::object tensor, py::tuple tuple_val) {
    size_t tuple_size = tuple_val.size();
    size_t ndim_sum = 0, cur_sum = 0;
    int pos = -1;
    bool has_unknown_ndim_bool_index = false;
    for (size_t i = 0; i < tuple_size; ++i) {
        py::object handle = tuple_val[i];
        if (handle.ptr() == Py_Ellipsis) {
            pos = static_cast<int>(i);
            for (size_t j = 0; j < i; ++j) {
                py::object t = tuple_val[j];
                if (t.ptr() == Py_Ellipsis) {
                    throw py::index_error("only one ellipsis is allowed.");
                }
            }
        } else {
            size_t ndim_incr = 1;
            if (hasattr(handle, "dtype") && is_bool_dtype(handle.ptr()) &&
                hasattr(handle, "ndim")) {
                py::object ndim = getattr(handle, "ndim");
                if (PyLong_Check(ndim.ptr())) {
                    ndim_incr = PyLong_AsLong(ndim.ptr());
                } else {
                    has_unknown_ndim_bool_index = true;
                }
            }
            cur_sum += ndim_incr;
        }
    }
    if (pos == -1) {
        return tuple_val;
    } else {
        if (has_unknown_ndim_bool_index) {
            throw py::index_error(
                    "does not support bool index with unknown shape when using "
                    "Ellipsis.");
        }
        try {
            ndim_sum = getattr(tensor, "ndim").cast<size_t>();
        } catch (py::error_already_set& err) {
            throw py::index_error(
                    "does not support Ellipsis when tensor's ndim is unknown.");
        }
        py::tuple ret(ndim_sum - cur_sum + tuple_size - 1);
        size_t idx = 0;
        for (size_t i = 0; i < tuple_size; ++i) {
            if (i == pos) {
                for (size_t j = cur_sum; j < ndim_sum; ++j) {
                    ret[idx++] = PySlice_New(NULL, NULL, NULL);
                }
            } else {
                ret[idx++] = tuple_val[i];
            }
        }
        return ret;
    }
}

py::tuple _expand_bool_dim(py::object tensor, py::tuple tuple_val) {
    py::tuple cur_shape = _make_shape_tuple(py::handle(getattr(tensor, "shape")));
    py::list new_tuple_val(0);

    size_t offset = 0;
    size_t tdim = 0;
    for (size_t i = 0; i < tuple_val.size(); ++i) {
        py::handle k = tuple_val[i];
        if (is_bool_dtype(k.ptr())) {
            size_t ndim = getattr(k, "ndim").cast<size_t>();
            if (ndim > 1) {
                py::tuple ishape = _make_shape_tuple(py::handle(getattr(k, "shape")));
                for (size_t j = 0; j < ndim; ++j) {
                    if (cur_shape[tdim + j - offset].cast<size_t>() !=
                        ishape[j].cast<size_t>()) {
                        std::string msg =
                                "boolean index did not match tensor along dimension " +
                                std::to_string(tdim + j) + "; dimension is " +
                                std::to_string(
                                        cur_shape[tdim + j - offset].cast<size_t>()) +
                                " but corresponding boolean dimension is " +
                                std::to_string(ishape[j].cast<size_t>());
                        throw py::index_error(msg.c_str());
                    }
                }
                py::object new_k = getattr(k, "reshape")(-1);
                py::object kshape = getattr(new_k, "shape");
                py::list new_shape(0);
                PyObject* sym = PyObject_CallObject(cpp_use_symbolic_shape, nullptr);
                bool is_sym = (sym == Py_True);
                Py_XDECREF(sym);
                if (is_sym) {
                    py::object tshape = getattr(tensor, "shape");
                    for (size_t j = 0; j < i; ++j) {
                        new_shape.append(tshape[py::int_(j)]);
                    }
                    new_shape.append(kshape[py::int_(0)]);
                    for (size_t j = tdim + ndim - offset; j < cur_shape.size(); ++j) {
                        new_shape.append(cur_shape[j]);
                    }
                    py::tuple args = py::make_tuple(new_shape);
                    PyObject* shape_tensor =
                            PyObject_CallObject(cpp_astensor1d, args.ptr());
                    py::object reshape_func = getattr(tensor, "reshape");
                    Py_INCREF(shape_tensor);
                    PyObject* Args = PyTuple_New(1);
                    PyTuple_SetItem(Args, 0, shape_tensor);
                    PyObject* new_tensor =
                            PyObject_CallObject(reshape_func.ptr(), Args);
                    Py_XDECREF(Args);
                    tensor = py::reinterpret_steal<py::object>(new_tensor);
                    cur_shape = _make_shape_tuple(py::handle(shape_tensor));
                    Py_XDECREF(shape_tensor);
                } else {
                    for (size_t j = 0; j < i; ++j) {
                        new_shape.append(cur_shape[j]);
                    }
                    new_shape.append(py::reinterpret_borrow<py::tuple>(kshape)[0]);
                    for (size_t j = tdim + ndim - offset; j < cur_shape.size(); ++j) {
                        new_shape.append(cur_shape[j]);
                    }
                    cur_shape = new_shape;
                    tensor = getattr(tensor, "reshape")(cur_shape);
                }
                offset++;
                tdim += ndim;
            }
            new_tuple_val.append(k);
        } else {
            new_tuple_val.append(k);
            tdim++;
        }
    }
    return py::make_tuple(tensor, py::reinterpret_borrow<py::tuple>(new_tuple_val));
}

py::tuple _unpack_indexes(py::handle inp_hdl, py::handle idx_hdl) {
    py::object inp = py::reinterpret_borrow<py::object>(inp_hdl);
    py::tuple tuple_val;
    if (py::isinstance<py::tuple>(idx_hdl)) {
        tuple_val = py::reinterpret_borrow<py::tuple>(idx_hdl);
    } else {
        tuple_val = py::make_tuple(idx_hdl);
    }

    bool use_subtensor = true;
    bool need_remove_ellipsis = false;
    bool need_expand_bool_dim = false;
    size_t idx_ndim = 0;
    for (size_t i = 0; i < tuple_val.size(); ++i) {
        py::object k = tuple_val[i];
        if (k.ptr() == Py_None) {
            throw py::index_error("newaxis is not allowed here");
        } else if (k.ptr() == Py_Ellipsis) {
            need_remove_ellipsis = true;
        } else {
            if (is_bool_dtype(k.ptr()) && hasattr(k, "ndim")) {
                size_t ndim = getattr(k, "ndim").cast<size_t>();
                idx_ndim += ndim;
                if (ndim > 1) {
                    need_expand_bool_dim = true;
                }
            } else {
                idx_ndim++;
            }
        }
    }
    try {
        size_t inp_ndim = getattr(inp, "ndim").cast<size_t>();
        if (idx_ndim > inp_ndim) {
            std::string msg = "too many indices for tensor: tensor is " +
                              std::to_string(inp_ndim) + "-dimensional, but " +
                              std::to_string(idx_ndim) + " were indexed";
            throw py::index_error(msg.c_str());
        }
    } catch (py::error_already_set& err) {
        ;  // ignore
    }
    if (need_remove_ellipsis) {
        tuple_val = _remove_ellipsis(inp, tuple_val);
    }

    if (need_expand_bool_dim) {
        py::object shape = getattr(inp, "shape");
        if (shape.ptr() != Py_None) {
            py::tuple ret = _expand_bool_dim(inp, tuple_val);
            inp = ret[0];
            tuple_val = ret[1];
        }
    }

    py::list items;
    py::list tensors;
    int cur_axis = -1;

    for (size_t i = 0; i < tuple_val.size(); ++i) {
        py::object handle = tuple_val[i];
        cur_axis++;
        if (!is_scalar(handle.ptr()) && !PySlice_Check(handle.ptr())) {
            use_subtensor = false;
        }
        py::list item;
        item.append(cur_axis);
        auto push = [&](PyObject* v) {
            if (v == Py_None) {
                item.append(false);
            } else {
                item.append(true);
                tensors.append(_get_index(py::reinterpret_borrow<py::object>(v), inp));
            }
        };

        if (PySlice_Check(handle.ptr())) {
            PySliceObject* s = (PySliceObject*)handle.ptr();
            if (s->start == Py_None && s->stop == Py_None && s->step == Py_None) {
                continue;
            }
            push(s->start);
            push(s->stop);
            push(s->step);
            item.append(false);
        } else {
            for (size_t j = 0; j < 3; j++)
                item.append(false);
            push(handle.ptr());
        }
        items.append(item);
    }

    return py::make_tuple(inp, tensors, items, use_subtensor, need_expand_bool_dim);
}

py::object _getitem_cpp(py::handle inp_hdl, py::handle idx_hdl) {
    py::tuple try_res = _try_cond_take(inp_hdl, idx_hdl);
    if (try_res.size() == 2) {
        return try_res[0];
    }
    py::tuple up = _unpack_indexes(inp_hdl, idx_hdl);
    py::object tensor = py::reinterpret_borrow<py::object>(up[0]);
    py::list tensors = py::reinterpret_borrow<py::list>(up[1]);
    py::list py_items = py::reinterpret_borrow<py::list>(up[2]);
    std::vector<std::tuple<int8_t, bool, bool, bool, bool>> cpp_items;
    for (size_t i = 0; i < py_items.size(); ++i) {
        py::list item = py::reinterpret_borrow<py::list>(py_items[i]);
        cpp_items.push_back(
                {item[0].cast<int8_t>(), item[1].cast<bool>(), item[2].cast<bool>(),
                 item[3].cast<bool>(), item[4].cast<bool>()});
    }
    static std::shared_ptr<OpDef> op;
    if (up[3].cast<bool>()) {
        op = Subtensor::make(cpp_items);
    } else {
        op = IndexingMultiAxisVec::make(cpp_items);
    }
    std::vector<PyObject*> p;
    p.resize(tensors.size() + 2);
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = tensor.ptr();
    for (size_t i = 0; i < tensors.size(); ++i) {
        p[i + 2] = tensors[i].ptr();
    }
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    return ret[0];
}

py::object _setitem_cpp(py::handle inp_hdl, py::handle idx_hdl, py::handle val_hdl) {
    py::object org_shape = getattr(inp_hdl, "shape");
    py::object val = py::reinterpret_borrow<py::object>(val_hdl);
    if (!TensorWrapper::try_cast(val.ptr()) && !py::isinstance<PySymbolVar>(val)) {
        val =
                _Const(val_hdl, getattr(inp_hdl, "dtype"), getattr(inp_hdl, "device"),
                       inp_hdl);
    }

    py::tuple up = _unpack_indexes(inp_hdl, idx_hdl);
    py::object tensor = py::reinterpret_borrow<py::object>(up[0]);
    py::list tensors = py::reinterpret_borrow<py::list>(up[1]);
    py::list py_items = py::reinterpret_borrow<py::list>(up[2]);
    std::vector<std::tuple<int8_t, bool, bool, bool, bool>> cpp_items;
    for (size_t i = 0; i < py_items.size(); ++i) {
        py::list item = py::reinterpret_borrow<py::list>(py_items[i]);
        cpp_items.push_back(
                {item[0].cast<int8_t>(), item[1].cast<bool>(), item[2].cast<bool>(),
                 item[3].cast<bool>(), item[4].cast<bool>()});
    }
    static std::shared_ptr<OpDef> op, set_op;
    if (up[3].cast<bool>()) {
        op = Subtensor::make(cpp_items);
    } else {
        op = IndexingMultiAxisVec::make(cpp_items);
    }
    std::vector<PyObject*> p;
    p.resize(tensors.size() + 2);
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = tensor.ptr();
    for (size_t i = 0; i < tensors.size(); ++i) {
        p[i + 2] = tensors[i].ptr();
    }
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    py::object tmp_result = ret[0];

    try {
        py::object value_tuple_shape = val.attr("_tuple_shape");
        py::object tmp_result_tuple_shape = tmp_result.attr("_tuple_shape");
        py::tuple value_shape = py::reinterpret_borrow<py::tuple>(value_tuple_shape);
        py::tuple tmp_result_shape =
                py::reinterpret_borrow<py::tuple>(tmp_result_tuple_shape);
        for (size_t i = 0; i < value_shape.size() && i < tmp_result_shape.size(); ++i) {
            size_t vs = value_shape[value_shape.size() - i - 1].cast<size_t>();
            size_t ts =
                    tmp_result_shape[tmp_result_shape.size() - i - 1].cast<size_t>();
            if (vs != 1 && vs != ts) {
                std::string lhs = "", rhs = "";
                for (size_t j = 0; j < tmp_result_shape.size(); ++j) {
                    lhs += std::to_string(tmp_result_shape[j].cast<size_t>());
                    if (j)
                        lhs += ",";
                }
                for (size_t j = 0; j < value_shape.size(); ++j) {
                    rhs += std::to_string(value_shape[j].cast<size_t>());
                    if (j)
                        rhs += ",";
                }
                throw py::value_error(
                        "cannot copy tensor with shape (" + rhs +
                        ") to subtensor with shape (" + lhs + ")");
            }
        }
    } catch (py::error_already_set& err) {
        ;
    }

    py::object broadcast_func = getattr(val, "_broadcast");
    PyObject* Args = PyTuple_New(1);
    PyTuple_SetItem(Args, 0, getattr(tmp_result, "shape").release().ptr());
    PyObject* new_val = PyObject_CallObject(broadcast_func.ptr(), Args);
    Py_XDECREF(Args);
    val = py::reinterpret_steal<py::object>(new_val);

    if (up[3].cast<bool>()) {
        set_op = SetSubtensor::make(cpp_items);
    } else {
        set_op = IndexingSetMultiAxisVec::make(cpp_items);
    }

    std::vector<PyObject*> q;
    q.resize(tensors.size() + 3);
    py::object Set_Op = py::cast(set_op);
    q[0] = Set_Op.ptr();
    q[1] = tensor.ptr();
    q[2] = val.ptr();
    for (size_t i = 0; i < tensors.size(); ++i) {
        q[i + 3] = tensors[i].ptr();
    }
    py::tuple result =
            py::reinterpret_steal<py::object>(py_apply(NULL, q.data(), q.size()));
    py::object res = result[0];

    if (up[4].cast<bool>()) {
        py::object reshape_func = getattr(res, "reshape");
        PyObject* Args = PyTuple_New(1);
        PyTuple_SetItem(Args, 0, org_shape.release().ptr());
        PyObject* new_tensor = PyObject_CallObject(reshape_func.ptr(), Args);
        Py_XDECREF(Args);
        res = py::reinterpret_steal<py::object>(new_tensor);
    }

    return res;
}

bool is_tensor_or_symbolvar(py::handle arg) {
    return bool(TensorWrapper::try_cast(arg.ptr())) || py::isinstance<PySymbolVar>(arg);
}

bool is_py_sequence(py::handle arg) {
    if (PyArray_Check(arg.ptr()) || TensorWrapper::try_cast(arg.ptr()) ||
        py::isinstance<PySymbolVar>(arg)) {
        return false;
    }
    return PySequence_Check(arg.ptr());
}

py::object _split_cpp(
        py::handle inp_hdl, py::handle nsplits_or_sections_hdl, py::handle axis_hdl) {
    py::object shape_obj = getattr(inp_hdl, "shape");
    py::object n_total = shape_obj[axis_hdl];
    int ndim = shape_obj.attr("__len__")().cast<int>();
    int axis = axis_hdl.cast<int>();
    if (axis >= ndim) {
        throw py::value_error("Invalid axis " + std::to_string(axis));
    }
    int n_sections;
    bool is_array;
    if (is_py_sequence(nsplits_or_sections_hdl)) {
        n_sections = PySequence_Length(nsplits_or_sections_hdl.ptr()) + 1;
        is_array = true;
    } else {
        n_sections = getattr(nsplits_or_sections_hdl, "__int__")().cast<int>();
        is_array = false;
    }
    py::list partitions;
    std::shared_ptr<OpDef> op;
    std::vector<PyObject*> p;
    if (is_array) {
        py::list div_points;
        py::list sections = py::reinterpret_borrow<py::object>(nsplits_or_sections_hdl);
        div_points.append(0);
        for (size_t i = 0; i < sections.size(); ++i) {
            div_points.append(sections[i]);
        }
        div_points.append(n_total);
        for (size_t i = 1; i < div_points.size(); ++i) {
            if (div_points[i - 1] > div_points[i]) {
                throw py::value_error(
                        "Invalid nsplits_or_secions: " +
                        repr(nsplits_or_sections_hdl).cast<std::string>());
            }
            py::object pos = div_points[i] - div_points[i - 1];
            if (is_tensor_or_symbolvar(pos)) {
                partitions.append(pos);
            } else {
                partitions.append(
                        _Const(pos, py::cast((mgb::DType)dtype::Int32()),
                               getattr(inp_hdl, "device"), inp_hdl));
            }
        }
        op = Split::make(axis, 0);
        p.resize(partitions.size() + 2);
        for (size_t i = 0; i < partitions.size(); ++i) {
            p[i + 2] = partitions[i].ptr();
        }
    } else {
        if (n_sections <= 0) {
            throw py::value_error("Number sections must be larger than 0");
        }
        if (py::int_(n_sections) > n_total) {
            throw py::value_error(
                    "The size " + repr(n_total).cast<std::string>() + " at dim " +
                    std::to_string(axis) + " cannot be split into " +
                    std::to_string(n_sections) + " sections");
        }
        op = Split::make(axis, n_sections);
        p.resize(2);
    }
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = inp_hdl.ptr();
    return py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
}

std::vector<int32_t> list2vector(py::handle li) {
    std::vector<int32_t> axis;
    if (is_py_sequence(li.ptr())) {
        py::list tmp_list = py::reinterpret_steal<py::list>(PySequence_List(li.ptr()));
        for (size_t i = 0; i < tmp_list.size(); ++i) {
            axis.push_back(tmp_list[i].attr("__int__")().cast<int32_t>());
        }
    } else {
        axis.push_back(getattr(li, "__int__")().cast<int32_t>());
    }
    return axis;
}

py::object _expand_dims_cpp(py::handle inp_hdl, py::handle axis_hdl) {
    std::vector<int32_t> axis = list2vector(axis_hdl);
    bool unknown_ndim = true;
    size_t ndim = axis.size();
    if (auto p = TensorWrapper::try_cast(inp_hdl.ptr())) {
        auto&& shape = p->m_tensor->shape();
        if (shape) {
            unknown_ndim = false;
            ndim += shape->ndim;
        }
    } else {
        auto&& var = inp_hdl.cast<PySymbolVar*>();
        auto&& mgr = var->m_node->owner_graph()->static_infer_manager();
        auto&& shape = mgr.infer_shape_fallible(var->m_node);
        if (shape) {
            unknown_ndim = false;
            ndim += shape->ndim;
        }
    }
    for (size_t i = 0; i < axis.size(); ++i) {
        if (axis[i] < 0) {
            if (unknown_ndim) {
                throw py::index_error(
                        "Does not support negative index when tensor's ndim is "
                        "unknown");
            }
            axis[i] += static_cast<int32_t>(ndim);
        }
    }
    if (!axis.size()) {
        throw py::index_error("axis could not be empty");
    }
    std::sort(axis.begin(), axis.end());
    std::shared_ptr<OpDef> op = AddAxis::make(axis = axis);
    std::vector<PyObject*> p;
    p.resize(2);
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = inp_hdl.ptr();
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    return ret[0];
}

py::object _squeeze_cpp(py::handle inp_hdl, py::handle axis_hdl) {
    std::vector<int32_t> axis;
    size_t ndim;
    if (axis_hdl.ptr() != Py_None) {
        axis = list2vector(axis_hdl);
    }
    if (auto p = TensorWrapper::try_cast(inp_hdl.ptr())) {
        auto&& shape = p->m_tensor->shape();
        if (shape) {
            ndim = shape->ndim;
            if (axis_hdl.ptr() == Py_None) {
                for (size_t i = 0; i < shape->ndim; ++i) {
                    if (shape->shape[i] == 1) {
                        axis.push_back(i);
                    }
                }
            }
        }
    } else {
        auto&& var = inp_hdl.cast<PySymbolVar*>();
        auto&& mgr = var->m_node->owner_graph()->static_infer_manager();
        auto&& shape = mgr.infer_shape_fallible(var->m_node);
        if (shape) {
            ndim = shape->ndim;
            if (axis_hdl.ptr() == Py_None) {
                for (size_t i = 0; i < shape->ndim; ++i) {
                    if (shape->shape[i] == 1) {
                        axis.push_back(i);
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < axis.size(); ++i) {
        if (axis[i] < 0) {
            axis[i] += static_cast<int32_t>(ndim);
        }
    }
    std::sort(axis.begin(), axis.end());
    for (size_t i = 0; i < axis.size(); ++i) {
        axis[i] -= static_cast<int32_t>(i);
    }
    std::shared_ptr<OpDef> op = RemoveAxis::make(axis = axis);
    std::vector<PyObject*> p;
    p.resize(2);
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = inp_hdl.ptr();
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    return ret[0];
}

size_t fast_ndim(py::handle tensor) {
    if (auto p = TensorWrapper::try_cast(tensor.ptr())) {
        return p->m_tensor->shape()->ndim;
    }
    return getattr(tensor, "ndim").cast<size_t>();
}

py::object _expand_args(py::handle args) {
    if (!PyTuple_Check(args.ptr())) {
        return py::reinterpret_borrow<py::object>(args);
    }
    py::tuple args_tup = py::reinterpret_borrow<py::tuple>(args.ptr());
    if (args_tup.size() == 1 && (PySequence_Check(args_tup[0].ptr()) ||
                                 is_tensor_or_symbolvar(args_tup[0].ptr()))) {
        return py::reinterpret_borrow<py::object>(args_tup[0]);
    } else {
        return py::reinterpret_steal<py::list>(PySequence_List(args_tup.ptr()));
    }
}

py::object _transpose_cpp(py::handle inp_hdl, py::handle args) {
    py::object obj = _expand_args(args);
    py::list lis;
    if (!is_tensor_or_symbolvar(obj.ptr()) && PySequence_Check(obj.ptr())) {
        lis = py::reinterpret_steal<py::list>(PySequence_List(obj.ptr()));
    } else {
        py::object np = getattr(obj, "numpy")();
        PyArrayObject* arr = (PyArrayObject*)np.ptr();
        PyObject* maybe_list = PyArray_ToList(arr);
        if (PyList_Check(maybe_list)) {
            lis = py::reinterpret_steal<py::list>(maybe_list);
        }
    }
    if (fast_ndim(inp_hdl) == 0) {
        if (lis.size() != 0) {
            throw py::index_error(
                    "transpose for scalar does not accept additional args");
        }
        return getattr(inp_hdl, "to")(getattr(inp_hdl, "device"));
    }
    std::vector<int32_t> pattern;
    if (!lis.size()) {
        size_t ndim = getattr(inp_hdl, "ndim").cast<size_t>();
        for (size_t i = 0; i < ndim; ++i) {
            pattern.push_back(ndim - i - 1);
        }
    } else {
        for (size_t i = 0; i < lis.size(); ++i) {
            if (PyLong_Check(lis[i].ptr())) {
                pattern.push_back(lis[i].cast<int32_t>());
            } else {
                if (lis[i].cast<std::string>() == "x") {
                    pattern.push_back(-1);
                }
            }
        }
    }
    std::shared_ptr<OpDef> op = Dimshuffle::make(pattern);
    std::vector<PyObject*> p;
    p.resize(2);
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = inp_hdl.ptr();
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    return ret[0];
}

std::tuple<std::vector<int32_t>, bool> tuple2vector(py::object shape) {
    std::vector<int32_t> shp;
    if (!PyTuple_Check(shape.ptr())) {
        return {shp, false};
    }
    py::tuple tup = py::reinterpret_borrow<py::tuple>(shape);
    for (size_t i = 0; i < tup.size(); ++i) {
        if (!PyLong_Check(tup[i].ptr())) {
            return {shp, false};
        } else {
            shp.push_back(tup[i].cast<int32_t>());
        }
    }
    return {shp, true};
}

bool enable_fastpath(py::handle inp) {
    if (!TensorWrapper::try_cast(inp.ptr()) ||
        TransformationManager::get_instance()
                        .segments[TransformationManager::Segment::Trace]
                        .size() > 0 ||
        TransformationManager::get_instance()
                        .segments[TransformationManager::Segment::ModuleTrace]
                        .size() > 0) {
        return false;
    }
    return true;
}

py::object _broadcast_cpp(py::handle inp_hdl, py::handle args) {
    py::object shape_hdl = _expand_args(args);
    bool auto_infer = false;
    py::list lis;
    py::list new_shape;
    if (PyList_Check(shape_hdl.ptr()) || PyTuple_Check(shape_hdl.ptr())) {
        lis = py::reinterpret_steal<py::list>(PySequence_List(shape_hdl.ptr()));
        for (size_t i = 0; i < lis.size(); ++i) {
            if (lis[i].ptr() == Py_None) {
                auto_infer = true;
                size_t right = lis.size() - i;
                py::object tshp = getattr(inp_hdl, "_tuple_shape");
                if (tshp.ptr() == Py_None) {
                    throw py::index_error("does not support `None` with unknown shape");
                }
                py::tuple inp_shape = py::reinterpret_borrow<py::tuple>(tshp);
                if (inp_shape.size() >= right) {
                    if (enable_fastpath(inp_hdl)) {
                        lis[i] = inp_shape[inp_shape.size() - right];
                    }
                    new_shape.append(inp_shape[inp_shape.size() - right]);
                } else {
                    throw py::value_error("invalid broadcast shape");
                }
            } else {
                new_shape.append(lis[i]);
                if (PyLong_Check(lis[i].ptr())) {
                    int32_t s = lis[i].cast<int32_t>();
                    if (s < 0) {
                        throw py::value_error(
                                "expect shape[" + std::to_string(i) +
                                "] >= 0 or use `None` to auto infer, got " +
                                std::to_string(s));
                    }
                }
            }
        }
    }
    if (auto_infer) {
        if (enable_fastpath(inp_hdl)) {
            shape_hdl = py::reinterpret_borrow<py::tuple>(lis);
        } else {
            py::tuple args = py::make_tuple(new_shape, inp_hdl);
            py::dict kwargs;
            kwargs["dtype"] = py::cast((mgb::DType)dtype::Int32());
            kwargs["device"] = getattr(inp_hdl, "device");
            shape_hdl = py::reinterpret_steal<py::object>(
                    PyObject_Call(cpp_astensor1d, args.ptr(), kwargs.ptr()));
        }
    }
    py::object shape_tuple;
    try {
        shape_tuple = _make_shape_tuple(shape_hdl);
    } catch (py::error_already_set& err) {
        shape_tuple = py::reinterpret_borrow<py::object>(shape_hdl);
    }
    auto [shape, fastpath] = tuple2vector(shape_tuple);
    fastpath &= enable_fastpath(inp_hdl);
    std::shared_ptr<OpDef> op;
    std::vector<PyObject*> p;
    py::object shape_tensor;
    if (fastpath) {
        op = Broadcast::make(shape);
        p.resize(2);
    } else {
        op = Broadcast::make();
        py::tuple args = py::make_tuple(shape_hdl, inp_hdl);
        py::dict kwargs;
        kwargs["dtype"] = py::cast((mgb::DType)dtype::Int32());
        kwargs["device"] = getattr(inp_hdl, "device");
        shape_tensor = py::reinterpret_steal<py::object>(
                PyObject_Call(cpp_astensor1d, args.ptr(), kwargs.ptr()));
        p.resize(3);
        p[2] = shape_tensor.ptr();
    }
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = inp_hdl.ptr();
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    return ret[0];
}

py::object _reshape_cpp(py::handle inp_hdl, py::handle args) {
    py::object shape_hdl = _expand_args(args);
    py::object shape_tuple;
    try {
        shape_tuple = _make_shape_tuple(shape_hdl);
    } catch (py::error_already_set& err) {
        shape_tuple = py::reinterpret_borrow<py::object>(shape_hdl);
    }
    int32_t unspec_axis = -1;
    if (PyTuple_Check(shape_tuple.ptr())) {
        py::tuple tup = py::reinterpret_borrow<py::tuple>(shape_tuple);
        for (size_t i = 0; i < tup.size(); ++i) {
            py::object obj = py::reinterpret_borrow<py::object>(tup[i]);
            if (obj < py::int_(0)) {
                if (obj.not_equal(py::int_(-1))) {
                    throw py::value_error(
                            "expect shape [" + std::to_string(i) + "] >= -1, got " +
                            repr(obj).cast<std::string>());
                }
                if (unspec_axis >= 0) {
                    throw py::value_error(
                            "multiple -1 in shape: " + std::to_string(unspec_axis) +
                            " & " + std::to_string(i));
                }
                unspec_axis = i;
            }
        }
    }
    auto [shape, fastpath] = tuple2vector(shape_tuple);
    fastpath &= enable_fastpath(inp_hdl);
    std::shared_ptr<OpDef> op;
    std::vector<PyObject*> p;
    py::object shape_tensor;
    if (fastpath) {
        if (unspec_axis >= 0) {
            op = Reshape::make(unspec_axis, shape);
        } else {
            op = Reshape::make(::megdnn::param::OptionalAxisV1::INVALID_AXIS, shape);
        }
        p.resize(2);
    } else {
        shape.clear();
        if (unspec_axis >= 0) {
            op = Reshape::make(unspec_axis, shape);
        } else {
            op = Reshape::make();
        }
        py::tuple args = py::make_tuple(shape_hdl, inp_hdl);
        py::dict kwargs;
        kwargs["dtype"] = py::cast((mgb::DType)dtype::Int32());
        kwargs["device"] = getattr(inp_hdl, "device");
        shape_tensor = py::reinterpret_steal<py::object>(
                PyObject_Call(cpp_astensor1d, args.ptr(), kwargs.ptr()));
        p.resize(3);
        p[2] = shape_tensor.ptr();
    }
    py::object Op = py::cast(op);
    p[0] = Op.ptr();
    p[1] = inp_hdl.ptr();
    py::tuple ret =
            py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
    return ret[0];
}

mgb::DType _get_dtype(py::handle tensor) {
    if (auto tw = TensorWrapper::try_cast(tensor.ptr())) {
        return tw->m_tensor->dtype();
    } else {
        auto var = tensor.cast<PySymbolVar*>();
        return var->m_node->dtype();
    }
}

py::object _astype_cpp(py::handle tensor, py::handle dtype_hdl) {
    PyArray_Descr* descr;
    if (!PyArray_DescrConverter(dtype_hdl.ptr(), &descr)) {
        throw py::value_error(ssprintf(
                "can not convert to numpy.dtype from %s",
                dtype_hdl.ptr()->ob_type->tp_name));
    }
    PyArray_Descr* cur = npy::dtype_mgb2np_descr(_get_dtype(tensor)).get();
    if (!dtype_equal(cur, descr)) {
        std::shared_ptr<OpDef> op = TypeCvt::make(npy::dtype_np2mgb_descr(descr));
        py::object Op = py::cast(op);
        std::vector<PyObject*> p;
        p.resize(2);
        p[0] = Op.ptr();
        p[1] = tensor.ptr();
        py::tuple ret =
                py::reinterpret_steal<py::object>(py_apply(NULL, p.data(), p.size()));
        return ret[0];
    } else {
        return py::reinterpret_borrow<py::object>(tensor);
    }
}

py::object _convert_single_value_cpp(
        py::handle value, py::handle dtype, py::handle device) {
    if (is_tensor_or_symbolvar(value)) {
        if (_get_dtype(value).category() != DTypeCategory::QUANTIZED) {
            return _astype_cpp(value, dtype);
        }
    } else {
        return _Const(value, dtype, device, py::none());
    }
    return py::reinterpret_borrow<py::object>(value);
}

py::object _convert_inputs_cpp(
        PyObject* const* args, size_t nargs, py::object dtype, py::object device) {
    ComputingGraph* graph = nullptr;
    py::handle typeobj;
    py::list lis;
    for (size_t i = 0; i < nargs; ++i) {
        py::handle h = py::handle(args[i]);
        lis.append(h);
        if (py::isinstance<PySymbolVar>(h)) {
            auto var = h.cast<PySymbolVar*>();
            auto g = var->m_node->owner_graph();
            if (!graph) {
                graph = g;
                typeobj = h.get_type();
            } else {
                mgb_assert(graph == g);
            }
        }
    }
    if (graph) {
        CompNode cn = device.cast<CompNode>();
        for (size_t i = 0; i < nargs; ++i) {
            OperatorNodeConfig config(cn);
            auto hv = npy::np2tensor(
                    lis[i].ptr(), npy::Meth::borrow(cn), dtype.cast<mgb::DType>());
            if (py::isinstance<PySymbolVar>(lis[i])) {
                lis[i] = typeobj(opr::ImmutableTensor::make(*graph, hv, config).node());
            }
        }
    }
    auto convert = [&](py::object value) {
        if (value.ptr() == Py_None) {
            return value;
        }
        return _convert_single_value_cpp(value, dtype, device);
    };
    for (size_t i = 0; i < lis.size(); ++i) {
        lis[i] = convert(lis[i]);
    }
    return py::reinterpret_steal<py::tuple>(PyList_AsTuple(lis.ptr()));
}

PyObject* make_shape_tuple(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _make_shape_tuple(py::handle(args[0])).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* getitem_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _getitem_cpp(py::handle(args[0]), py::handle(args[1])).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* setitem_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _setitem_cpp(
                       py::handle(args[0]), py::handle(args[1]), py::handle(args[2]))
                .release()
                .ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* split_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _split_cpp(py::handle(args[0]), py::handle(args[1]), py::handle(args[2]))
                .release()
                .ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* expand_dims_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _expand_dims_cpp(py::handle(args[0]), py::handle(args[1]))
                .release()
                .ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* squeeze_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _squeeze_cpp(py::handle(args[0]), py::handle(args[1])).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* transpose_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _transpose_cpp(py::handle(args[0]), py::handle(args[1])).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* broadcast_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _broadcast_cpp(py::handle(args[0]), py::handle(args[1])).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* reshape_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _reshape_cpp(py::handle(args[0]), py::handle(args[1])).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* Const(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _Const(py::handle(args[0]), py::handle(args[1]), py::handle(args[2]),
                      py::handle(args[3]))
                .release()
                .ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* astype_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _astype_cpp(py::handle(args[0]), py::handle(args[1])).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* convert_single_value_cpp(
        PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        return _convert_single_value_cpp(
                       py::handle(args[0]), py::handle(args[1]), py::handle(args[2]))
                .release()
                .ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

PyObject* convert_inputs_cpp(PyObject* self, PyObject* const* args, size_t nargs) {
    try {
        py::object dtype = py::reinterpret_steal<py::object>(
                dtype_promotion(self, args, nargs - 1));
        py::object device;
        if (args[nargs - 1] == Py_None) {
            device = py::reinterpret_steal<py::object>(
                    get_device(self, args, nargs - 1));
        } else {
            device = py::reinterpret_borrow<py::object>(args[nargs - 1]);
        }
        return _convert_inputs_cpp(args, nargs - 1, dtype, device).release().ptr();
    }
    PYEXT17_TRANSLATE_EXC_RET(nullptr)
}

}  // namespace mgb::imperative::python
