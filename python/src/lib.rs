//! cpython bindings for sparsemap backed by the pure rust port in ../rust

use pyo3::exceptions::{PyOverflowError, PyTypeError, PyValueError};
use pyo3::prelude::*;
use pyo3::types::{PyBytes, PyInt};

// leading colons keep the crate distinct from the generated pymodule item
use ::sparsemap::SparseMap as Map;

/// a sparse compressed run length encoded set of non negative integers
#[pyclass(name = "SparseMap", module = "sparsemap", eq)]
#[derive(Clone, Default, PartialEq)]
struct PySparseMap {
    inner: Map,
}

/// iterator over set bits ascending
#[pyclass(name = "SparseMapIter", module = "sparsemap")]
struct PySparseMapIter {
    items: std::vec::IntoIter<u64>,
}

#[pymethods]
impl PySparseMapIter {
    fn __iter__(slf: PyRef<'_, Self>) -> PyRef<'_, Self> {
        slf
    }

    fn __next__(&mut self) -> Option<u64> {
        self.items.next()
    }
}

#[pymethods]
impl PySparseMap {
    #[new]
    #[pyo3(signature = (items = None))]
    fn new(items: Option<&Bound<'_, PyAny>>) -> PyResult<Self> {
        let mut inner = Map::new();
        if let Some(items) = items {
            for item in items.try_iter()? {
                inner.insert(item?.extract::<u64>()?);
            }
        }
        Ok(PySparseMap { inner })
    }

    /// set bit idx and return true when it was newly set
    fn insert(&mut self, idx: u64) -> bool {
        self.inner.insert(idx)
    }

    /// clear bit idx and return true when it was set
    fn remove(&mut self, idx: u64) -> bool {
        self.inner.remove(idx)
    }

    /// remove every bit
    fn clear(&mut self) {
        self.inner.clear();
    }

    /// return true when bit idx is set
    fn contains(&self, idx: u64) -> bool {
        self.inner.contains(idx)
    }

    /// return the count of set bits because len itself can overflow
    fn cardinality(&self) -> u64 {
        self.inner.cardinality()
    }

    /// return the smallest set bit or none when empty
    fn min(&self) -> Option<u64> {
        self.inner.min()
    }

    /// return the largest set bit or none when empty
    fn max(&self) -> Option<u64> {
        self.inner.max()
    }

    /// return the count of set bits strictly below idx
    fn rank(&self, idx: u64) -> u64 {
        self.inner.rank(idx)
    }

    /// return the position of the nth set bit counting from zero
    fn select(&self, n: u64) -> Option<u64> {
        self.inner.select(n)
    }

    /// return the first start of length consecutive bits equal to value
    #[pyo3(signature = (start, length, value = true))]
    fn span(&self, start: u64, length: u64, value: bool) -> Option<u64> {
        self.inner.span(start, length, value)
    }

    /// set every bit in the half open range from start to end
    fn insert_range(&mut self, start: u64, end: u64) {
        self.inner.insert_range(start, end);
    }

    /// clear every bit in the half open range from start to end
    fn remove_range(&mut self, start: u64, end: u64) {
        self.inner.remove_range(start, end);
    }

    /// return a new map holding every bit set in either map
    fn union(&self, other: PyRef<'_, Self>) -> Self {
        PySparseMap {
            inner: self.inner.union(&other.inner),
        }
    }

    /// return a new map holding the bits set in both maps
    fn intersection(&self, other: PyRef<'_, Self>) -> Self {
        PySparseMap {
            inner: self.inner.intersection(&other.inner),
        }
    }

    /// return a new map holding bits set here but not in other
    fn difference(&self, other: PyRef<'_, Self>) -> Self {
        PySparseMap {
            inner: self.inner.difference(&other.inner),
        }
    }

    /// return a new map holding the bits set in exactly one map
    fn symmetric_difference(&self, other: PyRef<'_, Self>) -> Self {
        PySparseMap {
            inner: self.inner.symmetric_difference(&other.inner),
        }
    }

    /// return true when the maps share at least one set bit
    fn intersects(&self, other: PyRef<'_, Self>) -> bool {
        self.inner.intersects(&other.inner)
    }

    /// return true when every bit set here is set in other
    fn is_subset(&self, other: PyRef<'_, Self>) -> bool {
        self.inner.is_subset(&other.inner)
    }

    /// return true when every bit set in other is set here
    fn is_superset(&self, other: PyRef<'_, Self>) -> bool {
        self.inner.is_superset(&other.inner)
    }

    /// return a copy with every bit index shifted by offset
    fn shifted(&self, offset: i64) -> Self {
        PySparseMap {
            inner: self.inner.shifted(offset),
        }
    }

    /// serialize to the c compatible wire format
    fn to_bytes<'py>(&self, py: Python<'py>) -> Bound<'py, PyBytes> {
        PyBytes::new(py, &self.inner.to_bytes())
    }

    /// deserialize bytes produced by this class or the c library
    #[staticmethod]
    fn from_bytes(data: &[u8]) -> PyResult<Self> {
        Map::from_bytes(data)
            .map(|inner| PySparseMap { inner })
            .map_err(|e| PyValueError::new_err(format!("invalid sparsemap buffer: {e:?}")))
    }

    fn __contains__(&self, item: &Bound<'_, PyAny>) -> PyResult<bool> {
        if let Ok(idx) = item.extract::<u64>() {
            return Ok(self.inner.contains(idx));
        }
        // out of universe ints read as absent to match builtin set semantics
        if item.downcast::<PyInt>().is_ok() {
            return Ok(false);
        }
        Err(PyTypeError::new_err(
            "SparseMap members are non-negative integers",
        ))
    }

    fn __len__(&self) -> PyResult<usize> {
        usize::try_from(self.inner.cardinality())
            .map_err(|_| PyOverflowError::new_err("cardinality exceeds len(); use cardinality()"))
    }

    fn __bool__(&self) -> bool {
        !self.inner.is_empty()
    }

    fn __iter__(&self) -> PySparseMapIter {
        // snapshot since a borrowing iterator cannot outlive a python owned cell
        PySparseMapIter {
            items: self.inner.to_vec().into_iter(),
        }
    }

    fn __or__(&self, other: PyRef<'_, Self>) -> Self {
        self.union(other)
    }

    fn __and__(&self, other: PyRef<'_, Self>) -> Self {
        self.intersection(other)
    }

    fn __sub__(&self, other: PyRef<'_, Self>) -> Self {
        self.difference(other)
    }

    fn __xor__(&self, other: PyRef<'_, Self>) -> Self {
        self.symmetric_difference(other)
    }

    fn __ior__(&mut self, other: &Bound<'_, Self>) {
        // the borrow only fails for x with itself which is a no op
        if let Ok(o) = other.try_borrow() {
            self.inner |= &o.inner;
        }
    }

    fn __iand__(&mut self, other: &Bound<'_, Self>) {
        // the borrow only fails for x with itself which is a no op
        if let Ok(o) = other.try_borrow() {
            self.inner &= &o.inner;
        }
    }

    fn __isub__(&mut self, other: &Bound<'_, Self>) {
        match other.try_borrow() {
            Ok(o) => self.inner -= &o.inner,
            // the borrow only fails for x with itself which empties the set
            Err(_) => self.inner.clear(),
        }
    }

    fn __ixor__(&mut self, other: &Bound<'_, Self>) {
        match other.try_borrow() {
            Ok(o) => self.inner ^= &o.inner,
            // the borrow only fails for x with itself which empties the set
            Err(_) => self.inner.clear(),
        }
    }

    fn __repr__(&self) -> String {
        format!("SparseMap(cardinality={})", self.inner.cardinality())
    }
}

#[pymodule]
fn sparsemap(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<PySparseMap>()?;
    m.add_class::<PySparseMapIter>()?;
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;
    Ok(())
}
