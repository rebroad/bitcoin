// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/mempoolstats.h>
#include <qt/forms/ui_mempoolstats.h>
#include <QtMath>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>

static const char *LABEL_FONT = "Arial";
static int LABEL_TITLE_SIZE = 22;
static int LABEL_KV_SIZE = 12;

static const int LABEL_LEFT_SIZE = 30;
static const int LABEL_RIGHT_SIZE = 30;
static const int GRAPH_PADDING_LEFT = 30+LABEL_LEFT_SIZE;
static const int GRAPH_PADDING_RIGHT = 30+LABEL_RIGHT_SIZE;
static const int GRAPH_PADDING_TOP = 10;
static const int GRAPH_PADDING_TOP_LABEL = 10;
static const int GRAPH_PADDING_BOTTOM = 30;

void ClickableTextItem::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    Q_EMIT objectClicked(this);
}

void ClickableRectItem::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    Q_EMIT objectClicked(this);
}

MempoolStats::MempoolStats(QWidget *parent) : QWidget(parent), m_clientmodel(nullptr) {

    if (parent) {
        parent->installEventFilter(this);
        raise();
    }
    // autoadjust font size
    QGraphicsTextItem testText("jY"); //screendesign expected 27.5 pixel in width for this string
    testText.setFont(QFont(LABEL_FONT, LABEL_TITLE_SIZE, QFont::Light));
    LABEL_TITLE_SIZE *= 27.5/testText.boundingRect().width();
    LABEL_KV_SIZE *= 27.5/testText.boundingRect().width();

    m_gfx_view = new QGraphicsView(this);
    m_scene = new QGraphicsScene(m_gfx_view); // m_scene is a child of m_gfx_view explicitly
    m_gfx_view->setScene(m_scene);
    m_scene->setSceneRect(0, 0, width(), height());
    m_gfx_view->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    // drawChart will be called when the client model is set
}

void MempoolStats::setClientModel(ClientModel *model) {
    m_clientmodel = model;
    if (model) {
        connect(model, &ClientModel::mempoolFeeHistChanged, this, &MempoolStats::drawChart);
        // Draw chart after connections are made
        drawChart();
    }
}

// Define colors for fee ranges using a dynamic HSV-based approach
// This generates a spectrum of distinct colors that will accommodate various numbers of fee ranges
QColor getColorForRange(int index, int totalRanges) {
    // Use HSV color model for better visual distinction
    // Hue values chosen to provide good contrast (0-360 degrees)
    const double baseHue = 240.0; // Start with blue
    const double hueFactor = 360.0 / (totalRanges > 0 ? totalRanges : 1);

    // Calculate hue based on index (wrapping around the color wheel if needed)
    double hue = fmod(baseHue + index * hueFactor, 360.0);

    // Saturation and value settings for vibrant but not too bright colors
    const int saturation = 240;
    const int value = 220;

    return QColor::fromHsv(hue, saturation, value);
}

// Fallback static colors for backward compatibility - TODO: explain when fallball would occur.
const static std::vector<QColor> colors = { QColor("#535154"), QColor("#0000ac"), QColor("#0000c2"), QColor("#0000d8"), QColor("#0000ec"), QColor("#0000ff"), QColor("#2c2cff"), QColor("#5858ff"), QColor("#8080ff"),
                                            QColor("#008000"), QColor("#00a000"), QColor("#00c000"), QColor("#00e000"), QColor("#30e030"), QColor("#60e060"), QColor("#90e090"),
                                            QColor("#808000"), QColor("#989800"), QColor("#b0b000"), QColor("#c8c800"), QColor("#e0e000"), QColor("#e0e030"), QColor("#e0e060"),
                                            QColor("#800000"), QColor("#a00000"), QColor("#c00000"), QColor("#e00000"), QColor("#e02020"), QColor("#e04040"), QColor("#e06060"),
                                            QColor("#800080"), QColor("#ac00ac"), QColor("#d800d8"), QColor("#ff00ff"), QColor("#ff2cff"), QColor("#ff58ff"), QColor("#ff80ff"),
                                            QColor("#000000") };
// Add destructor for proper cleanup
MempoolStats::~MempoolStats() {
    if (m_scene) {
        m_scene->clear();  // Clear items before destruction
    }
    delete m_gfx_view; // This will also delete m_scene since it's a child
    m_scene = nullptr;
    m_gfx_view = nullptr;
}

QGraphicsTextItem* MempoolStats::createTextItem(const QString& text, const QFont& font) {
    if (text.isEmpty()) return nullptr;
    
    QGraphicsTextItem* item = nullptr;
    try {
        item = new QGraphicsTextItem();
        if (item) {
            item->setFont(font);
            item->setPlainText(text);
            m_scene->addItem(item);
        }
    } catch (const std::exception& e) {
        LogPrintf("Failed to create text item: %s\n", e.what());
        delete item;
        return nullptr;
    }
    return item;
}

void MempoolStats::drawChart() {
    // Early return if client model is null
    if (!m_clientmodel) {
        return;
    }

    // Use instance mutex for thread safety
    QMutexLocker drawLocker(&m_draw_mutex);

    // Early return if essential scene pointers are null
    if (!m_scene || !m_gfx_view) {
        return;
    }

    // If we're already drawing, skip
    if (drawing) {
        return;
    }

    // Guard against re-entrancy
    struct DrawingGuard {
        bool& drawing;
        DrawingGuard(bool& d) : drawing(d) { drawing = true; }
        ~DrawingGuard() { drawing = false; }
    } drawingGuard(drawing);

    try {
        // Declare all variables at the beginning of the function
        std::vector<QPainterPath> fee_paths;
        std::vector<size_t> fee_subtotal_totalnum;
        std::vector<size_t> fee_subtotal_num;
        qreal current_x = GRAPH_PADDING_LEFT;
        const qreal maxheight_g = (m_gfx_view->scene()->sceneRect().height()-GRAPH_PADDING_TOP-GRAPH_PADDING_TOP_LABEL-GRAPH_PADDING_BOTTOM);
        size_t max_num = 0;
        QFont gridFont;
        gridFont.setPointSize(8);
        int display_up_to_range = 0;
        qreal maxwidth = qMax(0.0, m_gfx_view->scene()->sceneRect().width()-GRAPH_PADDING_LEFT-GRAPH_PADDING_RIGHT);
        const qreal bottom = qMax(0.0, m_gfx_view->scene()->sceneRect().height()-GRAPH_PADDING_BOTTOM);
        size_t max_num_graph = 0;

        m_scene->disconnect();
        m_scene->clear();

        // We already checked m_clientmodel at the beginning of the function

        // Load saved stats if available
        try {
            // We already checked m_clientmodel at the beginning of the function

            FILE *filestr = nullptr;
            try {
                filestr = fsbridge::fopen("/tmp/statsdump", "rb");
                if (filestr) {
                    CAutoFile file(filestr, SER_DISK, false);
                    file >> m_clientmodel->m_mempool_feehist;
                    // file.fclose() will be called by CAutoFile destructor
                }
            } catch (const std::exception& e) {
                LogPrintf("Failed to load mempool stats: %s\n", e.what());
                // Ensure file is closed if exception occurred
                if (filestr) {
                    fclose(filestr);
                }
            }
        } catch (const std::exception& e) {
            LogPrintf("Error in mempool stats loading: %s\n", e.what());
        }

        std::vector<ClientModel::mempool_feehist_sample> feeHistCopy;
        {
            // Scope the mempool locker to minimize lock time
            QMutexLocker mempoolLocker(&m_clientmodel->m_mempool_locker);

            if (m_clientmodel->m_mempool_feehist.empty()) {
                return;  // draw nothing if no data
            }

            // Create a copy of the data we need while under lock
            feeHistCopy = m_clientmodel->m_mempool_feehist;
        }  // mempoolLocker is released here

        try {
            fee_subtotal_totalnum.resize(feeHistCopy[0].second.size());
            fee_subtotal_num.resize(feeHistCopy[0].second.size());
        } catch (const std::bad_alloc& e) {
            LogPrintf("Failed to allocate memory for fee subtotals: %s\n", e.what());
            return;
        }

        // Calculate max for y-axis
        for (const ClientModel::mempool_feehist_sample& sample : feeHistCopy) {
            uint64_t num = 0;
            int i = 0;
            for (const interfaces::mempool_feeinfo& list_entry : sample.second) {
                if (fCount) {
                    fee_subtotal_num[i] = list_entry.tx_count;
                    fee_subtotal_totalnum[i] += list_entry.tx_count;
                    num += list_entry.tx_count;
                } else {
                    fee_subtotal_num[i] = list_entry.total_size;
                    fee_subtotal_totalnum[i] += list_entry.total_size;
                    num += list_entry.total_size;
                }
                i++;
            }
            if (num > max_num) max_num = num;
        }

        // hide ranges we don't have txns
        for (size_t i = 0; i < fee_subtotal_totalnum.size(); i++)
            if (fee_subtotal_totalnum[i] > 0) display_up_to_range = i;

        // Pre-size the fee_paths vector to prevent memory corruption
        try {
            fee_paths.resize(display_up_to_range + 1);
        } catch (const std::bad_alloc& e) {
            LogPrintf("Failed to allocate memory for fee paths: %s\n", e.what());
            return;
        }

        // make a nice y-axis scale
        const int amount_of_h_lines = 4;
        if (max_num > 0) {
            int stepbase1 = qPow(10.0f, qFloor(log10(max_num))); // top value
            int stepbase2 = qPow(10.0f, qFloor(log10(1.0*max_num/amount_of_h_lines))); // first value
            int stepbase3 = qPow(10.0f, qFloor(log10(2.0*max_num/amount_of_h_lines))); // second value
            int step1 = (qCeil((1.0*max_num) / stepbase1) * stepbase1) / amount_of_h_lines;
            int step2 = qCeil((1.0*max_num/amount_of_h_lines) / stepbase2) * stepbase2;
            int step3 = qCeil((2.0*max_num/amount_of_h_lines) / stepbase3) * stepbase3 / 2;
            max_num_graph = std::min(std::min(step1,step2),step3)*amount_of_h_lines;
        }

        // calculate the x axis step per sample
        // we ignore the time difference of collected samples due to locking issues
        const qreal x_increment = 1.0 * (width()-GRAPH_PADDING_LEFT-GRAPH_PADDING_RIGHT) / m_clientmodel->m_mempool_max_samples; //samples.size();


        // draw horizontal grid
        QPainterPath grid_path(QPointF(current_x, bottom));
        for (int i=0; i <= amount_of_h_lines; i++) {
            qreal lY = bottom-i*(maxheight_g/amount_of_h_lines);
            grid_path.moveTo(GRAPH_PADDING_LEFT, lY);
            grid_path.lineTo(GRAPH_PADDING_LEFT+maxwidth, lY);

            size_t grid_num = static_cast<size_t>(i * (max_num_graph - m_bottom_num) / static_cast<double>(amount_of_h_lines)) + m_bottom_num;
            QGraphicsTextItem *item_num = nullptr;
            QString text = fCount ? QString::number(grid_num) : GUIUtil::formatBytes(grid_num);
            item_num = createTextItem(text, gridFont);
            if (item_num) {
                item_num->setPos(GRAPH_PADDING_LEFT+maxwidth, lY-(item_num->boundingRect().height()/2));
            }
        }

        QPen gridPen(QColor(100,100,100, 200), 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        m_scene->addPath(grid_path, gridPen);

        // draw fee ranges
        QGraphicsTextItem *fee_range_title = createTextItem("Fee ranges\n(sat/b)", gridFont);
        if (fee_range_title) {
            fee_range_title->setPos(2, bottom+10);
        }

        const qreal c_w = 10, c_h = 10, c_margin = 2;
        qreal c_y = bottom - c_margin;
        int i = 0;
        for (const interfaces::mempool_feeinfo& list_entry : feeHistCopy[0].second) {
            if (i > display_up_to_range) break; // Changed continue to break to prevent out-of-bounds access
            ClickableRectItem *fee_rect = nullptr;
            try {
                fee_rect = new ClickableRectItem();
            } catch (const std::bad_alloc& e) {
                LogPrintf("Failed to allocate memory for fee rectangle: %s\n", e.what());
                continue;
            }
            fee_rect->setRect(4, c_y, c_w, c_h);

            QColor brush_color = getColorForRange(i, display_up_to_range + 1);
            //QColor brush_color = colors[(i < static_cast<int>(colors.size()) ? i : static_cast<int>(colors.size())-1)];
            brush_color.setAlpha(85);
            if (m_selected_range >= 0 && m_selected_range != i)
                // if one item is selected, hide out the other ones
                brush_color.setAlpha(30);

            fee_rect->setBrush(QBrush(brush_color));
            fee_rect->setCursor(Qt::PointingHandCursor);
            connect(fee_rect, &ClickableRectItem::objectClicked, this, [this, i](QGraphicsItem*item) {
                // if clicked, we select or deselect if selected
                if (m_selected_range == i) m_selected_range = -1;
                else m_selected_range = i;

                QMetaObject::invokeMethod(this, &MempoolStats::drawChart, Qt::QueuedConnection);

                // TODO - make this happen on shutdown also
                try {
                    // Check for null m_clientmodel before accessing it
                    if (!m_clientmodel) {
                        LogPrintf("Cannot save mempool stats: client model is null\n");
                        return;
                    }

                    FILE *filestr = nullptr;
                    try {
                        filestr = fsbridge::fopen("/tmp/statsdump", "wb");
                        if (filestr) {
                            CAutoFile file(filestr, SER_DISK, false);
                            file << m_clientmodel->m_mempool_feehist;
                            // file.fclose() will be called by CAutoFile destructor
                        }
                    } catch (const std::exception& e) {
                        LogPrintf("Failed to save mempool stats: %s\n", e.what());
                        // Ensure file is closed if exception occurred
                        if (filestr) {
                            fclose(filestr);
                        }
                    }
                } catch (const std::exception& e) {
                    LogPrintf("Error in mempool stats saving: %s\n", e.what());
                }
            }, Qt::QueuedConnection);
            m_scene->addItem(fee_rect);

            ClickableTextItem *fee_text = nullptr;
            try {
                fee_text = new ClickableTextItem();
            } catch (const std::bad_alloc& e) {
                LogPrintf("Failed to allocate memory for fee text: %s\n", e.what());
                continue;
            }
            QString feeRangeText = QString::number(list_entry.fee_from)+"-"+QString::number(list_entry.fee_to);
            if (i+1 == static_cast<int>(m_clientmodel->m_mempool_feehist[0].second.size()))
                feeRangeText = QString::number(list_entry.fee_from)+"+";
            if (!safeSetText(fee_text, feeRangeText, gridFont)) {
                delete fee_text;
                continue;
            }
            fee_text->setPos(4+c_w+2, c_y);
            m_scene->addItem(fee_text);
            connect(fee_text, &ClickableTextItem::objectClicked, this, [this, fee_rect](QGraphicsItem*item) {
                fee_rect->objectClicked(item);
            }, Qt::QueuedConnection);

            c_y -= c_h + c_margin;
            i++;
        }

        // draw the paths
        bool first = true;
        for (const ClientModel::mempool_feehist_sample& sample : feeHistCopy) {
            current_x += x_increment;
            int i = 0;
            qreal y = bottom;
            for (const interfaces::mempool_feeinfo& list_entry : sample.second) {
                if (i > display_up_to_range) break; // Changed continue to break to prevent out-of-bounds access
                if (fCount) y -= (maxheight_g / max_num_graph * list_entry.tx_count);
                else y -= (maxheight_g / max_num_graph * list_entry.total_size);
                if (first) fee_paths.emplace_back(QPointF(current_x, y));
                else fee_paths[i].lineTo(current_x, y);
                i++;
            }
            first = false;
        }
        
        // Now process the collected data and draw the chart
        QString total_text = MempoolStats::tr("Last %1 hours").arg(QString::number(m_clientmodel->m_mempool_max_samples*m_clientmodel->m_mempool_collect_intervall/3600));
        i = 0;
        for (auto& feepath : fee_paths) {  // Use reference to avoid copying
            // close paths
            if (i > 0 && i < static_cast<int>(fee_paths.size()) && i-1 < static_cast<int>(fee_paths.size())) {
                // Check if the previous path has a valid position
                if (!fee_paths[i-1].isEmpty() && feepath.elementCount() > 0) {
                    //QPointF lastPoint = feepath.currentPosition();
                    feepath.lineTo(current_x, bottom);
                    feepath.lineTo(GRAPH_PADDING_LEFT, bottom);
                } else {
                    feepath.lineTo(current_x, bottom);
                    feepath.lineTo(GRAPH_PADDING_LEFT, bottom);
                }
            } else {
                feepath.lineTo(current_x, bottom);
                feepath.lineTo(GRAPH_PADDING_LEFT, bottom);
            }

            // Safe access to fee_subtotal_num
            QColor pen_color = getColorForRange(i, fee_paths.size());
            QColor brush_color = pen_color;
            pen_color.setAlpha(95);
            brush_color.setAlpha(85);
            if (m_selected_range >= 0 && m_selected_range != i) {
                pen_color.setAlpha(40);
                brush_color.setAlpha(30);
            }
            if (m_selected_range >= 0 && m_selected_range == i) {
                if (i < static_cast<int>(fee_subtotal_num.size())) {
                    if (fCount)
                        total_text = "transactions in selected fee range: "+QString::number(fee_subtotal_num[i]);
                    else
                        total_text = "bytes in selected fee range: "+GUIUtil::formatBytes(fee_subtotal_num[i]);
                }
            }
            QPen pen_blue(pen_color, 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            m_scene->addPath(feepath, pen_blue, QBrush(brush_color));
            i++;
        }

        QGraphicsTextItem *item_num = createTextItem(total_text, gridFont);
        if (item_num) {
            item_num->setPos(GRAPH_PADDING_LEFT+(maxwidth/2), bottom);
        }
    } catch (const std::exception& e) {
        if (m_scene) m_scene->clear();  // Clean up on error
        LogPrintf("MempoolStats::drawChart error: %s\n", e.what());
    }
}

// We override the virtual resizeEvent of the QWidget to adjust tables column
// sizes as the tables width is proportional to the dialogs width.
void MempoolStats::resizeEvent(QResizeEvent *event) {
    if (!event) return;

    try {
        QWidget::resizeEvent(event);
        if (m_gfx_view) {
            m_gfx_view->resize(size());
            if (m_scene) {
                m_scene->setSceneRect(0, 0, width(), height());
                drawChart();
            }
        }
    } catch (const std::exception& e) {
        LogPrintf("MempoolStats::resizeEvent error: %s\n", e.what());
    }
}

void MempoolStats::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (m_clientmodel)
        drawChart();
}

void MempoolStats::mousePressEvent(QMouseEvent *event) {
    QWidget::mousePressEvent(event);
    fCount = !fCount;
    if (m_clientmodel)
        drawChart();
}

bool MempoolStats::safeSetText(QGraphicsSimpleTextItem* item, const QString& text, const QFont& font) {
    if (!item || text.isEmpty()) return false;
    try {
        item->setFont(font);
        item->setText(text);
        return true;
    } catch (const std::exception& e) {
        LogPrintf("MempoolStats::safeSetText error: %s\n", e.what());
        return false;
    }
}
