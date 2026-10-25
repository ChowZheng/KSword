// QtWidgets interaction fixture. The runner replaces the marker below with
// InstallColumnPresets extracted from DiskMonitorStoragePanel.cpp. Only the
// shared theme/segmented paint and translation entry points are substituted;
// buttons, model, headers, menu events, and lifetimes use actual QtWidgets.
#include <QApplication>
#include <QTableView>
#include <QStandardItemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QSignalBlocker>
#include <QCryptographicHash>
#include <QTimer>
#include <QKeyEvent>
#include <QDebug>
#include <array>
#include <vector>
#include <algorithm>
#include <memory>
#include <cassert>
namespace ks::ui {
void InstallSegmentedButtons(QBoxLayout*, const QList<QAbstractButton*>&) {}
void SetSegmentedButtonSelected(QAbstractButton* b, bool v) {b->setProperty("selected",v);}
}
namespace ks::i18n {
QString sourceText(const QString& s) {return s;}
struct LanguageManager {static LanguageManager& instance(){static LanguageManager m;return m;}
void bindToolTip(QWidget* w,const QString&,const QString& s){w->setToolTip(s);}};
}
namespace KswordTheme {QString ContextMenuStyle(){return {};}}
// KSWORD_PRODUCTION_COLUMN_PRESETS

void clickMenu(QMenu* menu, int index) {
    auto* action=menu->actions().at(index);
    menu->setActiveAction(action);
    QKeyEvent press(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
    QApplication::sendEvent(menu,&press);
}
int main(int argc,char** argv){
    QApplication app(argc,argv);
    QWidget page; auto* layout=new QVBoxLayout(&page);auto* bar=new QHBoxLayout;
    layout->addLayout(bar);auto* view=new QTableView(&page);layout->addWidget(view);
    QStandardItemModel model(2,9);view->setModel(&model);
    disk_monitor_ui::InstallColumnPresets(bar,view,{0,1,2,3,4},{0,1,2,5,6,7,8},"A tip","B tip",0);
    page.show();app.processEvents();
    auto buttons=page.findChildren<QPushButton*>();assert(buttons.size()==2);
    auto* a=buttons[0];auto* b=buttons[1];
    assert(a->isChecked()&&!b->isChecked()&&!view->isColumnHidden(4)&&view->isColumnHidden(5));
    b->click();assert(!a->isChecked()&&b->isChecked()&&view->isColumnHidden(4)&&!view->isColumnHidden(5));
    QTimer::singleShot(0,[&](){auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());assert(menu);clickMenu(menu,2);});
    view->horizontalHeader()->customContextMenuRequested(QPoint(4,4));
    assert(!a->isChecked()&&!b->isChecked()&&view->isColumnHidden(0));
    assert(view->property("ksword_disk_column_preset").toInt()==-1);
    QTimer::singleShot(0,[&](){auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());assert(menu);clickMenu(menu,0);});
    view->horizontalHeader()->customContextMenuRequested(QPoint(4,4));
    for(int c=0;c<9;++c)assert(!view->isColumnHidden(c));
    assert(!a->isChecked()&&!b->isChecked());
    a->click();assert(a->isChecked()&&!b->isChecked()&&!view->isColumnHidden(4)&&view->isColumnHidden(5));
    QTimer::singleShot(0,[&](){auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());assert(menu);delete view;view=nullptr;menu->close();});
    auto* header=view->horizontalHeader();header->customContextMenuRequested(QPoint(4,4));assert(!view);
    qInfo()<<"PASS: A/B, custom unselect, all fields, menu/view lifetime";
}
