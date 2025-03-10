#include "tools/cabana/binaryview.h"

#include <cmath>

#include <QFontDatabase>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QShortcut>
#include <QToolTip>

#include "tools/cabana/commands.h"
#include "tools/cabana/signalview.h"

// BinaryView

const int CELL_HEIGHT = 36;
const int VERTICAL_HEADER_WIDTH = 30;

inline int get_bit_index(const QModelIndex &index, bool little_endian) {
  return index.row() * 8 + (little_endian ? 7 - index.column() : index.column());
}

BinaryView::BinaryView(QWidget *parent) : QTableView(parent) {
  model = new BinaryViewModel(this);
  setModel(model);
  delegate = new BinaryItemDelegate(this);
  setItemDelegate(delegate);
  horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  verticalHeader()->setSectionsClickable(false);
  verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  verticalHeader()->setDefaultSectionSize(CELL_HEIGHT);
  horizontalHeader()->hide();
  setShowGrid(false);
  setMouseTracking(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  QObject::connect(dbc(), &DBCManager::DBCFileChanged, this, &BinaryView::refresh);
  QObject::connect(UndoStack::instance(), &QUndoStack::indexChanged, this, &BinaryView::refresh);

  addShortcuts();
  setWhatsThis(R"(
    <b>Binary View</b><br/>
    <!-- TODO: add descprition here -->
    <span style="color:gray">Shortcuts</span><br />
    Delete Signal:
      <span style="background-color:lightGray;color:gray">&nbsp;x&nbsp;</span>,
      <span style="background-color:lightGray;color:gray">&nbsp;Backspace&nbsp;</span>,
      <span style="background-color:lightGray;color:gray">&nbsp;Delete&nbsp;</span><br />
    Change endianness: <span style="background-color:lightGray;color:gray">&nbsp;e&nbsp; </span><br />
    Change singedness: <span style="background-color:lightGray;color:gray">&nbsp;s&nbsp;</span><br />
    Open chart:
      <span style="background-color:lightGray;color:gray">&nbsp;c&nbsp;</span>,
      <span style="background-color:lightGray;color:gray">&nbsp;p&nbsp;</span>,
      <span style="background-color:lightGray;color:gray">&nbsp;g&nbsp;</span>
  )");
}

void BinaryView::addShortcuts() {
  // Delete (x, backspace, delete)
  QShortcut *shortcut_delete_x = new QShortcut(QKeySequence(Qt::Key_X), this);
  QShortcut *shortcut_delete_backspace = new QShortcut(QKeySequence(Qt::Key_Backspace), this);
  QShortcut *shortcut_delete_delete = new QShortcut(QKeySequence(Qt::Key_Delete), this);
  QObject::connect(shortcut_delete_delete, &QShortcut::activated, shortcut_delete_x, &QShortcut::activated);
  QObject::connect(shortcut_delete_backspace, &QShortcut::activated, shortcut_delete_x, &QShortcut::activated);
  QObject::connect(shortcut_delete_x, &QShortcut::activated, [=]{
    if (hovered_sig != nullptr) {
      emit removeSignal(hovered_sig);
      hovered_sig = nullptr;
    }
  });

  // Change endianness (e)
  QShortcut *shortcut_endian = new QShortcut(QKeySequence(Qt::Key_E), this);
  QObject::connect(shortcut_endian, &QShortcut::activated, [=]{
    if (hovered_sig != nullptr) {
      const cabana::Signal *hovered_sig_prev = hovered_sig;
      cabana::Signal s = *hovered_sig;
      s.is_little_endian = !s.is_little_endian;
      emit editSignal(hovered_sig, s);

      hovered_sig = nullptr;
      highlight(hovered_sig_prev);
    }
  });

  // Change signedness (s)
  QShortcut *shortcut_sign = new QShortcut(QKeySequence(Qt::Key_S), this);
  QObject::connect(shortcut_sign, &QShortcut::activated, [=]{
    if (hovered_sig != nullptr) {
      const cabana::Signal *hovered_sig_prev = hovered_sig;
      cabana::Signal s = *hovered_sig;
      s.is_signed = !s.is_signed;
      emit editSignal(hovered_sig, s);

      hovered_sig = nullptr;
      highlight(hovered_sig_prev);
    }
  });

  // Open chart (c, p, g)
  QShortcut *shortcut_plot = new QShortcut(QKeySequence(Qt::Key_P), this);
  QShortcut *shortcut_plot_g = new QShortcut(QKeySequence(Qt::Key_G), this);
  QShortcut *shortcut_plot_c = new QShortcut(QKeySequence(Qt::Key_C), this);
  QObject::connect(shortcut_plot_g, &QShortcut::activated, shortcut_plot, &QShortcut::activated);
  QObject::connect(shortcut_plot_c, &QShortcut::activated, shortcut_plot, &QShortcut::activated);
  QObject::connect(shortcut_plot, &QShortcut::activated, [=]{
    if (hovered_sig != nullptr) {
      emit showChart(model->msg_id, hovered_sig, true, false);
    }
  });
}

QSize BinaryView::minimumSizeHint() const {
  return {(horizontalHeader()->minimumSectionSize() + 1) * 9 + VERTICAL_HEADER_WIDTH + 2,
          CELL_HEIGHT * std::min(model->rowCount(), 10) + 2};
}

void BinaryView::highlight(const cabana::Signal *sig) {
  if (sig != hovered_sig) {
    for (int i = 0; i < model->items.size(); ++i) {
      auto &item_sigs = model->items[i].sigs;
      if ((sig && item_sigs.contains(sig)) || (hovered_sig && item_sigs.contains(hovered_sig))) {
        auto index = model->index(i / model->columnCount(), i % model->columnCount());
        emit model->dataChanged(index, index, {Qt::DisplayRole});
      }
    }

    hovered_sig = sig;
    emit signalHovered(hovered_sig);
  }
}

void BinaryView::setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags flags) {
  auto index = indexAt(viewport()->mapFromGlobal(QCursor::pos()));
  if (!anchor_index.isValid() || !index.isValid())
    return;

  QItemSelection selection;
  auto [start, size, is_lb] = getSelection(index);
  for (int i = start; i < start + size; ++i) {
    auto idx = model->bitIndex(i, is_lb);
    selection.merge({idx, idx}, flags);
  }
  selectionModel()->select(selection, flags);
}

void BinaryView::mousePressEvent(QMouseEvent *event) {
  delegate->selection_color = (palette().color(QPalette::Active, QPalette::Highlight));
  if (auto index = indexAt(event->pos()); index.isValid() && index.column() != 8) {
    anchor_index = index;
    auto item = (const BinaryViewModel::Item *)anchor_index.internalPointer();
    int bit_idx = get_bit_index(anchor_index, true);
    for (auto s : item->sigs) {
      if (bit_idx == s->lsb || bit_idx == s->msb) {
        anchor_index = model->bitIndex(bit_idx == s->lsb ? s->msb : s->lsb, true);
        resize_sig = s;
        delegate->selection_color = s->color;
        break;
      }
    }
  }
  event->accept();
}

void BinaryView::highlightPosition(const QPoint &pos) {
  if (auto index = indexAt(viewport()->mapFromGlobal(pos)); index.isValid()) {
    auto item = (BinaryViewModel::Item *)index.internalPointer();
    const cabana::Signal *sig = item->sigs.isEmpty() ? nullptr : item->sigs.back();
    highlight(sig);
  }
}

void BinaryView::mouseMoveEvent(QMouseEvent *event) {
  highlightPosition(event->globalPos());
  QTableView::mouseMoveEvent(event);
}

void BinaryView::mouseReleaseEvent(QMouseEvent *event) {
  QTableView::mouseReleaseEvent(event);

  auto release_index = indexAt(event->pos());
  if (release_index.isValid() && anchor_index.isValid()) {
    if (selectionModel()->hasSelection()) {
      auto [start_bit, size, is_lb] = getSelection(release_index);
      resize_sig ? emit resizeSignal(resize_sig, start_bit, size)
                 : emit addSignal(start_bit, size, is_lb);
    } else {
      auto item = (const BinaryViewModel::Item *)anchor_index.internalPointer();
      if (item && item->sigs.size() > 0)
        emit signalClicked(item->sigs.back());
    }
  }
  clearSelection();
  anchor_index = QModelIndex();
  resize_sig = nullptr;
}

void BinaryView::leaveEvent(QEvent *event) {
  highlight(nullptr);
  QTableView::leaveEvent(event);
}

void BinaryView::setMessage(const MessageId &message_id) {
  model->msg_id = message_id;
  verticalScrollBar()->setValue(0);
  refresh();
}

void BinaryView::refresh() {
  clearSelection();
  anchor_index = QModelIndex();
  resize_sig = nullptr;
  hovered_sig = nullptr;
  model->refresh();
  highlightPosition(QCursor::pos());
}

QSet<const cabana::Signal *> BinaryView::getOverlappingSignals() const {
  QSet<const cabana::Signal *> overlapping;
  for (auto &item : model->items) {
    if (item.sigs.size() > 1)
      for (auto s : item.sigs) overlapping += s;
  }
  return overlapping;
}

std::tuple<int, int, bool> BinaryView::getSelection(QModelIndex index) {
  if (index.column() == 8) {
    index = model->index(index.row(), 7);
  }
  bool is_lb = true;
  if (resize_sig) {
    is_lb = resize_sig->is_little_endian;
  } else if (settings.drag_direction == Settings::DragDirection::MsbFirst) {
    is_lb = index < anchor_index;
  } else if (settings.drag_direction == Settings::DragDirection::LsbFirst) {
    is_lb = !(index < anchor_index);
  } else if (settings.drag_direction == Settings::DragDirection::AlwaysLE) {
    is_lb = true;
  } else if (settings.drag_direction == Settings::DragDirection::AlwaysBE) {
    is_lb = false;
  }

  int cur_bit_idx = get_bit_index(index, is_lb);
  int anchor_bit_idx = get_bit_index(anchor_index, is_lb);
  auto [start_bit, end_bit] = std::minmax(cur_bit_idx, anchor_bit_idx);
  return {start_bit, end_bit - start_bit + 1, is_lb};
}

// BinaryViewModel

void BinaryViewModel::refresh() {
  beginResetModel();
  items.clear();
  if (auto dbc_msg = dbc()->msg(msg_id)) {
    row_count = dbc_msg->size;
    items.resize(row_count * column_count);
    for (auto sig : dbc_msg->getSignals()) {
      auto [start, end] = getSignalRange(sig);
      for (int j = start; j <= end; ++j) {
        int bit_index = sig->is_little_endian ? bigEndianBitIndex(j) : j;
        int idx = column_count * (bit_index / 8) + bit_index % 8;
        if (idx >= items.size()) {
          qWarning() << "signal " << sig->name << "out of bounds.start_bit:" << sig->start_bit << "size:" << sig->size;
          break;
        }
        if (j == start) sig->is_little_endian ? items[idx].is_lsb = true : items[idx].is_msb = true;
        if (j == end) sig->is_little_endian ? items[idx].is_msb = true : items[idx].is_lsb = true;

        auto &sigs = items[idx].sigs;
        sigs.push_back(sig);
        if (sigs.size() > 1) {
          std::sort(sigs.begin(), sigs.end(), [](auto l, auto r) { return l->size > r->size; });
        }
      }
    }
  } else {
    row_count = can->lastMessage(msg_id).dat.size();
    items.resize(row_count * column_count);
  }
  int valid_rows = std::min(can->lastMessage(msg_id).dat.size(), row_count);
  for (int i = 0; i < valid_rows * column_count; ++i) {
    items[i].valid = true;
  }
  endResetModel();
  updateState();
}

void BinaryViewModel::updateItem(int row, int col, const QString &val, const QColor &color) {
  auto &item = items[row * column_count + col];
  if (item.val != val || item.bg_color != color) {
    item.val = val;
    item.bg_color = color;
    auto idx = index(row, col);
    emit dataChanged(idx, idx, {Qt::DisplayRole});
  }
}

void BinaryViewModel::updateState() {
  const auto &last_msg = can->lastMessage(msg_id);
  const auto &binary = last_msg.dat;
  // data size may changed.
  if (binary.size() > row_count) {
    beginInsertRows({}, row_count, binary.size() - 1);
    row_count = binary.size();
    items.resize(row_count * column_count);
    endInsertRows();
  }

  const double max_f = 255.0;
  const double factor = 0.25;
  const double scaler = max_f / log2(1.0 + factor);
  for (int i = 0; i < binary.size(); ++i) {
    for (int j = 0; j < 8; ++j) {
      auto &item = items[i * column_count + j];
      QString val = ((binary[i] >> (7 - j)) & 1) != 0 ? "1" : "0";
      // Bit update frequency based highlighting
      double offset = !item.sigs.empty() ? 50 : 0;
      auto n = last_msg.bit_change_counts[i][7 - j];
      double min_f = n == 0 ? offset : offset + 25;
      double alpha = std::clamp(offset + log2(1.0 + factor * (double)n / (double)last_msg.count) * scaler, min_f, max_f);
      auto color = item.bg_color;
      color.setAlpha(alpha);
      updateItem(i, j, val, color);
    }
    updateItem(i, 8, toHex(binary[i]), last_msg.colors[i]);
  }
}

QVariant BinaryViewModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation == Qt::Vertical) {
    switch (role) {
      case Qt::DisplayRole: return section;
      case Qt::SizeHintRole: return QSize(VERTICAL_HEADER_WIDTH, 0);
      case Qt::TextAlignmentRole: return Qt::AlignCenter;
    }
  }
  return {};
}

QVariant BinaryViewModel::data(const QModelIndex &index, int role) const {
  if (role == Qt::ToolTipRole) {
    auto item = (const BinaryViewModel::Item *)index.internalPointer();
    if (item && !item->sigs.empty()) {
      return signalToolTip(item->sigs.back());
    }
  }
  return {};
}

// BinaryItemDelegate

BinaryItemDelegate::BinaryItemDelegate(QObject *parent) : QStyledItemDelegate(parent) {
  small_font.setPixelSize(8);
  hex_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  hex_font.setBold(true);
}

bool BinaryItemDelegate::hasSignal(const QModelIndex &index, int dx, int dy, const cabana::Signal *sig) const {
  if (!index.isValid()) return false;
  auto model = (const BinaryViewModel*)(index.model());
  int idx = (index.row() + dy) * model->columnCount() + index.column() + dx;
  return (idx >=0 && idx < model->items.size()) ? model->items[idx].sigs.contains(sig) : false;
}

void BinaryItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
  auto item = (const BinaryViewModel::Item *)index.internalPointer();
  BinaryView *bin_view = (BinaryView *)parent();
  painter->save();

  if (index.column() == 8) {
    if (item->valid) {
      painter->setFont(hex_font);
      painter->fillRect(option.rect, item->bg_color);
    }
  } else if (option.state & QStyle::State_Selected) {
    painter->fillRect(option.rect, selection_color);
    painter->setPen(option.palette.color(QPalette::BrightText));
  } else if (!bin_view->selectionModel()->hasSelection() || !item->sigs.contains(bin_view->resize_sig)) {  // not resizing
    if (item->sigs.size() > 0) {
      for (auto &s : item->sigs) {
        if (s == bin_view->hovered_sig) {
          painter->fillRect(option.rect, s->color.darker(125));  // 4/5x brightness
        } else {
          drawSignalCell(painter, option, index, s);
        }
      }
    } else if (item->valid) {
      painter->fillRect(option.rect, item->bg_color);
    }
    auto color_role = item->sigs.contains(bin_view->hovered_sig) ? QPalette::BrightText : QPalette::Text;
    painter->setPen(option.palette.color(color_role));
  }

  if (item->sigs.size() > 1) {
    painter->fillRect(option.rect, QBrush(Qt::darkGray, Qt::Dense7Pattern));
  } else if (!item->valid) {
    painter->fillRect(option.rect, QBrush(Qt::darkGray, Qt::BDiagPattern));
  }
  painter->drawText(option.rect, Qt::AlignCenter, item->val);
  if (item->is_msb || item->is_lsb) {
    painter->setFont(small_font);
    painter->drawText(option.rect.adjusted(8, 0, -8, -3), Qt::AlignRight | Qt::AlignBottom, item->is_msb ? "M" : "L");
  }
  painter->restore();
}

// Draw border on edge of signal
void BinaryItemDelegate::drawSignalCell(QPainter *painter, const QStyleOptionViewItem &option,
                                        const QModelIndex &index, const cabana::Signal *sig) const {
  bool draw_left = !hasSignal(index, -1, 0, sig);
  bool draw_top = !hasSignal(index, 0, -1, sig);
  bool draw_right = !hasSignal(index, 1, 0, sig);
  bool draw_bottom = !hasSignal(index, 0, 1, sig);

  const int spacing = 2;
  QRect rc = option.rect.adjusted(draw_left * 3, draw_top * spacing, draw_right * -3, draw_bottom * -spacing);
  QRegion subtract;
  if (!draw_top) {
    if (!draw_left && !hasSignal(index, -1, -1, sig)) {
      subtract += QRect{rc.left(), rc.top(), 3, spacing};
    } else if (!draw_right && !hasSignal(index, 1, -1, sig)) {
      subtract += QRect{rc.right() - 2, rc.top(), 3, spacing};
    }
  }
  if (!draw_bottom) {
    if (!draw_left && !hasSignal(index, -1, 1, sig)) {
      subtract += QRect{rc.left(), rc.bottom() - (spacing - 1), 3, spacing};
    } else if (!draw_right && !hasSignal(index, 1, 1, sig)) {
      subtract += QRect{rc.right() - 2, rc.bottom() - (spacing - 1), 3, spacing};
    }
  }
  painter->setClipRegion(QRegion(rc).subtracted(subtract));

  auto item = (const BinaryViewModel::Item *)index.internalPointer();
  QColor color = sig->color;
  color.setAlpha(item->bg_color.alpha());
  // Mixing the signal colour with the Base background color to fade it
  painter->fillRect(rc, QApplication::palette().color(QPalette::Base));
  painter->fillRect(rc, color);

  // Draw edges
  color = sig->color.darker(125);
  painter->setPen(QPen(color, 1));
  if (draw_left) painter->drawLine(rc.topLeft(), rc.bottomLeft());
  if (draw_right) painter->drawLine(rc.topRight(), rc.bottomRight());
  if (draw_bottom) painter->drawLine(rc.bottomLeft(), rc.bottomRight());
  if (draw_top) painter->drawLine(rc.topLeft(), rc.topRight());

  if (!subtract.isEmpty()) {
    // fill gaps inside corners.
    painter->setPen(QPen(color, 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    for (auto &r : subtract) {
      painter->drawRect(r);
    }
  }
}
